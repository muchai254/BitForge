#include "psbt_scan.h"
#include <Arduino.h>
#include <mbedtls/base64.h>
#include <string.h>
#include "camlink.h"
#include "display.h"
#include "seedqr.h"
#include "touch.h"
#include "ui.h"
#include "src/ur/ur.h"

// Layout (portrait 240 x 320).
#define BAR_X 20
#define BAR_Y 150
#define BAR_W 200
#define BAR_H 16
#define STATUS_Y 182
#define COUNT_Y 128
// Redraw at most this often: drawing keeps the loop away from the UART
// (see scan_screen.cpp), and frames arrive ~1 per second anyway.
#define REDRAW_MS 250

static const Button cancelButton = {12, 276, 100, 38, "Cancel", COL_GREY, COL_BLACK, 2};
static const Button restartButton = {128, 276, 100, 38, "Restart", COL_GREY, COL_BLACK, 2};
static const char *const PSBT_TYPES[] = {"crypto-psbt", "psbt", nullptr};
static const uint8_t PSBT_MAGIC[5] = {'p', 's', 'b', 't', 0xff};

static void drawStatus(const char *text, uint16_t color) {
  tft.fillRect(0, STATUS_Y, tft.width(), 80, COL_BLACK);
  drawTextWrapped(text, 12, STATUS_Y, 216, 1, color);
}

static void drawProgress(const ur::Decoder &d, uint32_t frames) {
  uint32_t total = d.seqLen(), known = d.fragmentsKnown();
  tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, COL_BLACK);
  if (total) tft.fillRect(BAR_X + 1, BAR_Y + 1, (BAR_W - 2) * known / total, BAR_H - 2, COL_BTC_ORANGE);
  char line[48];
  if (total) snprintf(line, sizeof line, "%lu of %lu parts  (%lu frames read)", (unsigned long)known,
                      (unsigned long)total, (unsigned long)frames);
  else snprintf(line, sizeof line, "%lu frames read", (unsigned long)frames);
  tft.fillRect(0, COUNT_Y, tft.width(), 12, COL_BLACK);
  drawCentredText(line, COUNT_Y, 1, COL_WHITE);
}

static void drawScreen() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Scan PSBT", 8, 2, COL_BTC_ORANGE);
  drawTextWrapped("In Sparrow: Finalize > Show QR. Set the QR density to the lowest (fewer, simpler frames). "
                  "Hold the camera steady on the animated QR.",
                  12, 40, 216, 1, COL_GREY);
  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_GREY);
  drawButton(cancelButton, false);
  drawButton(restartButton, false);
}

// The PSBT from a single base64 QR. False if it isn't valid base64.
static bool decodeBase64(const uint8_t *p, size_t n, uint8_t **out, size_t *outLen) {
  size_t need = 0;
  mbedtls_base64_decode(nullptr, 0, &need, p, n); // sizes the output
  if (need == 0 || need > PSBT_MAX_BYTES) return false;
  uint8_t *buf = (uint8_t *)malloc(need);
  if (!buf) return false;
  if (mbedtls_base64_decode(buf, need, outLen, p, n) != 0) {
    free(buf);
    return false;
  }
  *out = buf;
  return true;
}

// Takes the PSBT out of a completed UR message. False if the CBOR isn't one
// byte string.
static bool takeFromUr(const ur::Decoder &d, uint8_t **out, size_t *outLen) {
  const uint8_t *body;
  size_t n;
  if (!ur::unwrapByteString(d.message(), d.messageLen(), &body, &n) || n == 0 || n > PSBT_MAX_BYTES) return false;
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) return false;
  memcpy(buf, body, n);
  *out = buf;
  *outLen = n;
  return true;
}

bool psbtScanScreen(uint8_t **psbt, size_t *len) {
  *psbt = nullptr;
  *len = 0;
  ur::Decoder decoder(PSBT_TYPES);
  drawScreen();
  drawProgress(decoder, 0);
  drawStatus("Waiting for the camera...", COL_GREY);
  camlinkReset();

  uint32_t frames = 0, lastDraw = 0, startMs = millis();
  uint32_t otherInARow = 0;
  bool dirty = false, done = false, stuck = false;
  int held = -1; // button being pressed: 0 cancel, 1 restart
  const char *status = nullptr;
  uint16_t statusColor = COL_GREY;

  while (!done) {
    while (!done && camlinkPoll()) {
      const uint8_t *p = camlinkPayload();
      size_t n = camlinkPayloadLen();
      frames++;
      dirty = true;
      if (seedqrLooksLike(p, n)) {
        // Never log anything about it (see scan_screen.cpp).
        status = "That looks like a SeedQR. It's ignored here: show the PSBT QR instead.";
        statusColor = COL_BTC_ORANGE;
        continue;
      }
      if (stuck) continue; // waiting for Restart or Cancel
      if (n >= 7 && memcmp(p, "cHNidP8", 7) == 0 && decoder.partsAccepted() == 0) {
        if (decodeBase64(p, n, psbt, len)) {
          Serial.printf("[psbt] scan: single base64 QR, %u bytes\n", (unsigned)*len);
          done = true;
        } else {
          status = "Base64 PSBT QR could not be decoded.";
          statusColor = COL_RED;
        }
        continue;
      }
      ur::Status st = decoder.receive((const char *)p, n);
      switch (st) {
        case ur::ACCEPTED:
          otherInARow = 0;
          status = "Reading...";
          statusColor = COL_GREY;
          break;
        case ur::DUPLICATE:
          otherInARow = 0;
          break;
        case ur::COMPLETE:
          if (takeFromUr(decoder, psbt, len)) {
            Serial.printf("[psbt] scan: ur:%s, %lu parts from %lu frames, %u bytes, %lu ms\n", decoder.type(),
                          (unsigned long)decoder.seqLen(), (unsigned long)frames, (unsigned)*len,
                          (unsigned long)(millis() - startMs));
            done = true;
          } else {
            status = "The QR finished, but it doesn't hold a PSBT. Tap Restart.";
            statusColor = COL_RED;
            stuck = true;
          }
          break;
        case ur::FAILED:
          Serial.println("[psbt] scan: all parts arrived but the message checksum is wrong");
          status = "All parts arrived but the checksum is wrong. Tap Restart.";
          statusColor = COL_RED;
          stuck = true;
          break;
        case ur::OTHER_MESSAGE:
          // Sparrow shows a new QR when the transaction changes. Don't mix
          // them: after a few frames of the other one, ask to restart.
          if (++otherInARow >= 3) {
            status = "A different PSBT is being shown. Tap Restart to read the new one.";
            statusColor = COL_BTC_ORANGE;
          }
          break;
        case ur::WRONG_TYPE:
          status = "That QR isn't a PSBT (wrong UR type).";
          statusColor = COL_BTC_ORANGE;
          break;
        case ur::TOO_LARGE:
          status = "This PSBT is over the size limit (64 KB, 1024 parts).";
          statusColor = COL_RED;
          break;
        case ur::NO_MEMORY:
          status = "Out of memory. Tap Restart.";
          statusColor = COL_RED;
          stuck = true;
          break;
        case ur::NOT_UR:
        case ur::INVALID:
        case ur::ALREADY_DONE:
          status = "Not a PSBT QR.";
          statusColor = COL_BTC_ORANGE;
          break;
      }
      if (st != ur::DUPLICATE && st != ur::ACCEPTED && st != ur::COMPLETE) {
        Serial.printf("[psbt] scan: frame %lu refused (%s)\n", (unsigned long)frames, ur::statusName(st));
      }
    }
    if (done) break;

    uint32_t now = millis();
    if (dirty && now - lastDraw >= REDRAW_MS) {
      drawProgress(decoder, frames);
      if (status) drawStatus(status, statusColor);
      dirty = false;
      lastDraw = millis();
    }

    TouchEvent e = touchPoll();
    if (e.type == TOUCH_PRESS) {
      held = buttonHit(cancelButton, e.x, e.y) ? 0 : buttonHit(restartButton, e.x, e.y) ? 1 : -1;
      if (held == 0) drawButton(cancelButton, true);
      if (held == 1) drawButton(restartButton, true);
    } else if (e.type == TOUCH_RELEASE && held >= 0) {
      if (held == 0) {
        Serial.printf("[psbt] scan cancelled (%lu frames)\n", (unsigned long)frames);
        decoder.reset();
        camlinkWipe(); // in case a SeedQR was shown to the camera
        return false;
      }
      Serial.println("[psbt] scan restarted");
      decoder.reset();
      frames = otherInARow = 0;
      stuck = false;
      status = "Waiting for the camera...";
      statusColor = COL_GREY;
      drawButton(restartButton, false);
      camlinkReset();
      dirty = true;
      held = -1;
    }
    delay(5);
  }

  decoder.reset();
  camlinkWipe(); // in case a SeedQR was shown to the camera
  // Transport checks: the parser does the rest.
  if (*len < sizeof PSBT_MAGIC || memcmp(*psbt, PSBT_MAGIC, sizeof PSBT_MAGIC) != 0) {
    Serial.println("[psbt] scan: data doesn't start with the PSBT magic");
    free(*psbt);
    *psbt = nullptr;
    *len = 0;
    messageScreen("Not a PSBT", "The scanned data doesn't start with the PSBT header. Nothing was signed.");
    return false;
  }
  return true;
}
