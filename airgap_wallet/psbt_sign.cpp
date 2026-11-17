#include "psbt_sign.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include <Hash.h>
#include <PSBT.h>
#include "display.h"
#include "psbt_flow.h"
#include "qr_out.h"
#include "secure_mem.h"
#include "session.h"
#include "touch.h"
#include "ui.h"
#include "src/ur/ur.h"

// Noinline, so all key material sits in frames below the caller and
// secure_scrub_stack() in the caller wipes it. The destructors of root and of
// the keys inside PSBT::sign zero them too (uBitcoin memzero, patch 7 in docs/patches.md).
__attribute__((noinline)) static uint8_t signWithSeed(PSBT &lib, uint8_t fp[4], void (*progress)(float)) {
  HDPrivateKey root(session.mnemonic.c_str(), session.mnemonic.size(), session.passphrase.c_str(),
                    session.passphrase.size(), &Signet, progress);
  root.fingerprint(fp);
  if (memcmp(fp, session.fingerprint, 4) != 0) return 0;
  return lib.sign(root);
}

// Every input must have exactly one signature, made by the key we verified,
// over this transaction and the verified amount.
static bool checkSignatures(PSBT &lib, const PsbtInfo &info, const Review &r, char *error, size_t cap) {
  for (uint32_t i = 0; i < info.inputCount; i++) {
    const PSBTInputMetadata &m = lib.txInsMeta[i];
    if (m.signaturesLen != 1) {
      snprintf(error, cap, "Input %lu: %u signatures.", (unsigned long)(i + 1), m.signaturesLen);
      return false;
    }
    const PSBTPartialSignature &s = m.signatures[0];
    uint8_t sec[33];
    if (s.pubkey.sec(sec, sizeof sec) != 33 || memcmp(sec, info.inputs[i].derivations[0].pubkey, 33) != 0) {
      snprintf(error, cap, "Input %lu: signed by the wrong key.", (unsigned long)(i + 1));
      return false;
    }
    uint8_t h[32];
    lib.tx.sigHashSegwit(h, (uint8_t)i, s.pubkey.script(), r.inputAmounts[i]);
    if (!s.pubkey.verify(s.signature, h)) {
      snprintf(error, cap, "Input %lu: the signature doesn't verify.", (unsigned long)(i + 1));
      return false;
    }
  }
  return true;
}

static uint8_t *putKv(uint8_t *p, uint8_t type, const uint8_t *keyData, uint8_t keyDataLen, const uint8_t *a,
                      uint8_t aLen, const uint8_t *b, uint8_t bLen) {
  *p++ = (uint8_t)(keyDataLen + 1);
  *p++ = type;
  if (keyDataLen) memcpy(p, keyData, keyDataLen);
  p += keyDataLen;
  *p++ = (uint8_t)(aLen + bLen);
  memcpy(p, a, aLen);
  p += aLen;
  if (bLen) memcpy(p, b, bLen);
  return p + bLen;
}

// Unsigned tx; per input witness_utxo + partial signature; empty output maps.
static uint8_t *buildOutput(PSBT &lib, const PsbtInfo &info, const Review &r, size_t *outLen) {
  size_t cap = 5 + 2 + 3 + info.txRawLen + 1 + info.inputCount * (34 + 110 + 1) + info.outputCount;
  uint8_t *buf = (uint8_t *)malloc(cap);
  if (!buf) return nullptr;
  uint8_t *p = buf;
  memcpy(p, "psbt\xff", 5);
  p += 5;
  *p++ = 1;
  *p++ = 0x00;
  if (info.txRawLen < 0xfd) {
    *p++ = (uint8_t)info.txRawLen;
  } else {
    *p++ = 0xfd;
    *p++ = (uint8_t)info.txRawLen;
    *p++ = (uint8_t)(info.txRawLen >> 8);
  }
  memcpy(p, info.txRaw, info.txRawLen);
  p += info.txRawLen;
  *p++ = 0;
  for (uint32_t i = 0; i < info.inputCount; i++) {
    const uint8_t *pub = info.inputs[i].derivations[0].pubkey;
    uint8_t utxo[8 + 1 + 22];
    for (int k = 0; k < 8; k++) utxo[k] = (uint8_t)(r.inputAmounts[i] >> (8 * k));
    utxo[8] = 22;
    utxo[9] = 0x00;
    utxo[10] = 0x14;
    hash160(pub, 33, utxo + 11);
    p = putKv(p, 0x01, nullptr, 0, utxo, sizeof utxo, nullptr, 0);
    uint8_t der[80];
    size_t derLen = lib.txInsMeta[i].signatures[0].signature.serialize(der, sizeof der);
    const uint8_t sighash = 0x01; // SIGHASH_ALL
    if (derLen == 0 || derLen > 72) {
      free(buf);
      return nullptr;
    }
    p = putKv(p, 0x02, pub, 33, der, (uint8_t)derLen, &sighash, 1);
    *p++ = 0;
  }
  for (uint32_t i = 0; i < info.outputCount; i++) *p++ = 0;
  *outLen = (size_t)(p - buf);
  return buf;
}

uint8_t *psbtSign(const PsbtInfo &info, const Review &r, size_t *outLen, char *error, size_t errorCap,
                  void (*progress)(float)) {
  error[0] = 0;
  size_t cleanLen;
  uint8_t *clean = psbtBuildClean(info, r, &cleanLen);
  if (!clean) {
    snprintf(error, errorCap, "Out of memory.");
    return nullptr;
  }
  PSBT lib;
  uint8_t *out = nullptr;
  if (lib.parse(clean, cleanLen) != cleanLen || !lib.isValid()) {
    snprintf(error, errorCap, "The signing library couldn't read the transaction.");
  } else {
    uint8_t fp[4];
    uint8_t n = signWithSeed(lib, fp, progress);
    secure_scrub_stack();
    if (n != info.inputCount) {
      snprintf(error, errorCap, "Signed %u of %lu inputs.", n, (unsigned long)info.inputCount);
    } else if (checkSignatures(lib, info, r, error, errorCap)) {
      out = buildOutput(lib, info, r, outLen);
      if (!out) snprintf(error, errorCap, "Couldn't build the signed PSBT.");
    }
  }
  secure_zero(clean, cleanLen);
  free(clean);
  return out;
}

#define QR_Y 22
#define QR_BOX 232
#define FRAME_MS 400 // time each frame stays up, after drawing it

// Largest fragment per density setting: lower = fewer modules, easier for a webcam.
static const uint32_t DENSITY[] = {60, 100, 160};
static const char *const DENSITY_NAME[] = {"Low", "Med", "High"};

void psbtShowSignedScreen(const uint8_t *psbt, size_t len) {
  size_t cborCap = len + 5;
  uint8_t *cbor = (uint8_t *)malloc(cborCap);
  const size_t textCap = 1024;
  char *text = (char *)malloc(textCap);
  ur::Encoder *enc = new ur::Encoder();
  if (!cbor || !text) {
    free(cbor);
    free(text);
    delete enc;
    messageScreen("Error", "Out of memory showing the signed PSBT.");
    return;
  }
  size_t cborLen = ur::wrapByteString(psbt, len, cbor);

  const Button sizeBtn = {8, 276, 108, 40, "Density", COL_GREY, COL_BLACK, 2};
  const Button doneBtn = {124, 276, 108, 40, "Done", COL_BTC_ORANGE, COL_BLACK, 2};
  int density = 0;
  bool restart = true, running = true;
  bool logged = false;
  int held = -1;
  uint32_t frames = 0;
  while (running) {
    if (restart) {
      enc->begin("crypto-psbt", cbor, cborLen, DENSITY[density]);
      tft.fillScreen(COL_BLACK);
      drawButton(sizeBtn, false);
      drawButton(doneBtn, false);
      restart = false;
      logged = false;
    }
    int n = enc->nextPart(text, textCap);
    if (n < 0) break;
    for (int i = 0; i < n; i++) text[i] = (char)toupper((unsigned char)text[i]); // QR alphanumeric mode
    uint32_t t0 = millis();
    QrInfo q = qrDraw((const uint8_t *)text, n, 4, QR_Y, QR_BOX, QR_BOX, QR_ECC_LOW);
    uint32_t drawMs = millis() - t0;
    char line[48];
    snprintf(line, sizeof line, "Part %lu  (%lu parts, %s density)", (unsigned long)enc->seqNum(),
             (unsigned long)enc->seqLen(), DENSITY_NAME[density]);
    tft.fillRect(0, 4, tft.width(), 12, COL_BLACK);
    drawCentredText(line, 6, 1, COL_WHITE);
    if (!logged) {
      Serial.printf("[psbt] signed QR: %u bytes, %lu parts, %d chars per frame, v%d, %d px/module, drawn in %lu ms\n",
                    (unsigned)len, (unsigned long)enc->seqLen(), n, q.version, q.modulePx, (unsigned long)drawMs);
      logged = true;
    }
    frames++;
    // Hold the frame, watching the buttons.
    uint32_t until = millis() + FRAME_MS;
    do {
      TouchEvent e = touchPoll();
      if (e.type == TOUCH_PRESS) {
        held = buttonHit(sizeBtn, e.x, e.y) ? 0 : buttonHit(doneBtn, e.x, e.y) ? 1 : -1;
        if (held == 0) drawButton(sizeBtn, true);
        if (held == 1) drawButton(doneBtn, true);
      } else if (e.type == TOUCH_RELEASE && held >= 0) {
        if (held == 1) running = false;
        else {
          density = (density + 1) % 3;
          restart = true;
        }
        held = -1;
      }
      delay(5);
    } while (running && !restart && (int32_t)(until - millis()) > 0);
  }
  Serial.printf("[psbt] signed QR closed after %lu frames\n", (unsigned long)frames);
  secure_zero(cbor, cborCap);
  free(cbor);
  secure_zero(text, textCap);
  free(text);
  delete enc;
}
