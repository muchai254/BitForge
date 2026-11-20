#include "psbt_flow.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include <Hash.h>
#include <PSBT.h>
#include <mbedtls/sha256.h>
#include "display.h"
#include "keys.h"
#include "psbt_parse.h"
#include "psbt_review_screen.h"
#include "psbt_scan.h"
#include "psbt_sign.h"
#include "secure_mem.h"
#include "session.h"
#include "ui.h"

void psbtCheck(const uint8_t *psbt, size_t len, const HDPublicKey &account, const uint8_t fp[4], PsbtInfo &info,
               Review &r) {
  size_t pos = 0;
  PsbtParseError e = psbtParse(psbt, len, info, &pos);
  if (e != PSBT_OK) {
    memset(&r, 0, sizeof r);
    snprintf(r.reason, sizeof r.reason, "Invalid PSBT: %s (at byte %u).", psbtParseErrorText(e), (unsigned)pos);
    return;
  }
  psbtReview(info, account, fp, r);
  if (!r.ok) return;
  // The signing library must read the same transaction we verified.
  size_t cleanLen;
  uint8_t *clean = psbtBuildClean(info, r, &cleanLen);
  bool same = clean != nullptr;
  if (same) {
    PSBT lib;
    same = lib.parse(clean, cleanLen) == cleanLen && lib.isValid() && lib.tx.inputsNumber == info.inputCount &&
           lib.tx.outputsNumber == info.outputCount;
    for (uint32_t i = 0; same && i < info.inputCount; i++) same = lib.txInsMeta[i].txOut.amount == r.inputAmounts[i];
    if (same) {
      uint8_t *ser = (uint8_t *)malloc(info.txRawLen);
      same = ser && lib.tx.length() == info.txRawLen && lib.tx.serialize(ser, info.txRawLen) == info.txRawLen &&
             memcmp(ser, info.txRaw, info.txRawLen) == 0;
      free(ser);
    }
  }
  free(clean);
  if (!same) {
    r.ok = false;
    snprintf(r.reason, sizeof r.reason, "Invalid PSBT: the signing library can't read it the same way.");
  }
}

// Appends one key-value pair with a one-byte key length and value length.
static uint8_t *putKv(uint8_t *p, uint8_t type, const uint8_t *keyData, uint8_t keyDataLen, const uint8_t *val,
                      uint8_t valLen) {
  *p++ = (uint8_t)(keyDataLen + 1);
  *p++ = type;
  if (keyDataLen) memcpy(p, keyData, keyDataLen);
  p += keyDataLen;
  *p++ = valLen;
  memcpy(p, val, valLen);
  return p + valLen;
}

uint8_t *psbtBuildClean(const PsbtInfo &info, const Review &r, size_t *outLen) {
  // magic, global map (key + 3-byte length + tx + separator), then per input
  // witness_utxo (2 + 1 + 31), derivation (35 + 1 + 24), separator; 1 byte
  // per output map.
  size_t cap = 5 + 2 + 5 + info.txRawLen + 1 + info.inputCount * (34 + 60 + 1) + info.outputCount;
  uint8_t *buf = (uint8_t *)malloc(cap);
  if (!buf) return nullptr;
  uint8_t *p = buf;
  memcpy(p, "psbt\xff", 5);
  p += 5;
  *p++ = 1; // key length
  *p++ = 0x00; // unsigned tx
  if (info.txRawLen < 0xfd) {
    *p++ = (uint8_t)info.txRawLen;
  } else {
    *p++ = 0xfd;
    *p++ = (uint8_t)info.txRawLen;
    *p++ = (uint8_t)(info.txRawLen >> 8);
  }
  memcpy(p, info.txRaw, info.txRawLen);
  p += info.txRawLen;
  *p++ = 0; // end of the global map
  for (uint32_t i = 0; i < info.inputCount; i++) {
    const PsbtDerivation &d = info.inputs[i].derivations[0]; // verified: exactly one, and it's ours
    uint8_t utxo[8 + 1 + 22];
    uint64_t amount = r.inputAmounts[i];
    for (int k = 0; k < 8; k++) utxo[k] = (uint8_t)(amount >> (8 * k));
    utxo[8] = 22;
    utxo[9] = 0x00;
    utxo[10] = 0x14;
    hash160(d.pubkey, 33, utxo + 11);
    p = putKv(p, 0x01, nullptr, 0, utxo, sizeof utxo);
    uint8_t der[4 + 4 * 5];
    memcpy(der, d.fingerprint, 4);
    for (int k = 0; k < 5; k++) {
      for (int b = 0; b < 4; b++) der[4 + 4 * k + b] = (uint8_t)(d.path[k] >> (8 * b));
    }
    p = putKv(p, 0x06, d.pubkey, 33, der, sizeof der);
    *p++ = 0;
  }
  for (uint32_t i = 0; i < info.outputCount; i++) *p++ = 0;
  *outLen = (size_t)(p - buf);
  return buf;
}

static int progressPct = -1;
static void progressBar(float f) {
  int pct = (int)(f * 100);
  if (pct / 5 == progressPct / 5) return;
  progressPct = pct;
  tft.fillRect(20, 200, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

// Derives the loaded seed's public account key and self-checks it like the
// address screen does. False on failure.
static bool loadAccount(HDPublicKey &account) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Checking", 20, 2, COL_BTC_ORANGE);
  drawCentredText("Deriving the account...", 150, 1, COL_WHITE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;
  AccountInfo *a = (AccountInfo *)calloc(1, sizeof(AccountInfo));
  if (!a) return false;
  keysAccount(session.mnemonic.c_str(), session.mnemonic.size(), session.passphrase.c_str(), session.passphrase.size(),
              *a, progressBar);
  secure_scrub_stack();
  account = HDPublicKey(a->tpub);
  char addr[80];
  account.child(0).child(0).address(addr, sizeof addr);
  bool ok = strcmp(addr, a->receive[0]) == 0;
  free(a);
  return ok;
}

static void logReview(const Review &r) {
  if (!r.ok) {
    Serial.printf("[psbt] REFUSED: %s\n", r.reason);
    return;
  }
  Serial.printf("[psbt] verified: %lu in / %lu out; inputs %llu, outputs %llu, fee %llu sat, ~%lu vB, %lu.%lu sat/vB; "
                "amounts %s; high fee %s; RBF %s; locktime %lu\n",
                (unsigned long)r.inputCount, (unsigned long)r.outputCount, (unsigned long long)r.inputTotal,
                (unsigned long long)r.outputTotal, (unsigned long long)r.fee, (unsigned long)r.vsize,
                (unsigned long)(r.feeRateX10 / 10), (unsigned long)(r.feeRateX10 % 10),
                r.amountsVerified ? "verified" : "NOT verified (single input, witness_utxo)",
                r.highFee ? r.highFeeWhy : "no", r.rbf ? "on" : "off", (unsigned long)r.locktime);
  for (uint32_t i = 0; i < r.outputCount; i++) {
    const ReviewOutput &o = r.outputs[i];
    Serial.printf("[psbt]   output %lu: %llu sat to %s (%s%s)\n", (unsigned long)(i + 1), (unsigned long long)o.amount,
                  o.address, o.kind == OUT_CHANGE ? "change #" : o.kind == OUT_SELF ? "self, receive #" : "recipient",
                  o.kind == OUT_RECIPIENT ? (o.claimFailed ? ", CLAIMS to be ours but isn't" : "")
                                          : String(o.index).c_str());
  }
}

static void signAndShow(const PsbtInfo &info, const Review &r) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Signing", 20, 2, COL_BTC_ORANGE);
  drawCentredText("Deriving the keys...", 150, 1, COL_WHITE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;
  uint32_t t0 = millis();
  char error[80];
  size_t outLen = 0;
  uint8_t *out = psbtSign(info, r, &outLen, error, sizeof error, progressBar);
  if (!out) {
    Serial.printf("[psbt] signing FAILED: %s\n", error);
    psbtRefusedScreen(error);
    return;
  }
  // The txid (public), to compare with Sparrow and a block explorer.
  uint8_t h[32], h2[32];
  mbedtls_sha256(info.txRaw, info.txRawLen, h, 0);
  mbedtls_sha256(h, 32, h2, 0);
  char txid[65];
  for (int i = 0; i < 32; i++) snprintf(txid + 2 * i, 3, "%02x", h2[31 - i]);
  Serial.printf("[psbt] signed %lu input(s), all signatures verified, %lu ms; txid %s; loop stack %u B free at the low point\n",
                (unsigned long)info.inputCount, (unsigned long)(millis() - t0), txid,
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
  psbtShowSignedScreen(out, outLen);
  secure_zero(out, outLen);
  free(out);
}

void signPsbtScreen() {
  if (!session.loaded) return;
  uint8_t *psbt;
  size_t len;
  if (!psbtScanScreen(&psbt, &len)) return;
  Serial.printf("[psbt] received %u bytes\n", (unsigned)len);

  PsbtInfo *info = (PsbtInfo *)malloc(sizeof(PsbtInfo));
  Review *r = (Review *)malloc(sizeof(Review));
  HDPublicKey account;
  if (!info || !r) {
    free(info); free(r); free(psbt);
    psbtRefusedScreen("Out of memory.");
    return;
  }
  if (!loadAccount(account)) {
    free(info); free(r); free(psbt);
    psbtRefusedScreen("Account self-check failed.");
    return;
  }
  uint32_t t0 = millis();
  psbtCheck(psbt, len, account, session.fingerprint, *info, *r);
  Serial.printf("[psbt] checked in %lu ms\n", (unsigned long)(millis() - t0));
  logReview(*r);
  if (!r->ok) {
    psbtRefusedScreen(r->reason);
  } else if (psbtReviewScreens(*r)) {
    Serial.println("[psbt] approved by the user");
    signAndShow(*info, *r);
  } else {
    Serial.println("[psbt] rejected by the user");
    messageScreen("Rejected", "Nothing was signed.");
  }
  // Whatever happened, nothing about this transaction stays in memory.
  secure_zero(info, sizeof(PsbtInfo));
  free(info);
  secure_zero(psbt, len);
  free(psbt);
  secure_zero(r, sizeof(Review));
  free(r);
  size_t scrubbed = secure_scrub_free_heap();
  Serial.printf("[psbt] buffers zeroed, %u B of free heap zeroed\n", (unsigned)scrubbed);
  Serial.printf("[psbt] heap after this scan: %u B free, largest block %u B, lowest ever %u B\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}
