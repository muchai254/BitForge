#include "scan_screen.h"
#include "camlink.h"
#include "display.h"
#include "seedqr.h"
#include "touch.h"
#include "ui.h"

// Layout (portrait 240 x 320, text size 1 = 6 x 8 px, 10 px line pitch).
#define INFO_Y      26
#define TEXT_TOP    42
#define TEXT_LINES  19
#define TEXT_COLS   39   // 39 * 6 = 234 px
#define STATS_Y     236
#define COL_DIM     0x52AA
// Every frame is parsed and counted, but the payload is redrawn at most this
// often (always the newest one). Redrawing every frame of a fast stream kept
// the loop away from the UART long enough to lose bytes.
#define REDRAW_MS   250

static uint32_t maxDrawMs = 0;

static const Button backButton = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};

// Draws the payload character for character, hard-wrapped (no word wrap, so
// nothing is dropped or moved). Non-printable bytes are shown as '.'.
static void drawPayload(uint32_t count) {
  const uint8_t *p = camlinkPayload();
  size_t len = camlinkPayloadLen();
  tft.fillRect(0, INFO_Y, tft.width(), TEXT_TOP + TEXT_LINES * 10 - INFO_Y, COL_BLACK);
  if (seedqrLooksLike(p, len)) {
    // Never display (or log) anything that may be a seed on a test screen.
    char info[48];
    snprintf(info, sizeof info, "#%lu  %u bytes: looks like a SeedQR", (unsigned long)count, (unsigned)len);
    tft.setTextSize(1);
    tft.setTextColor(COL_BTC_ORANGE);
    tft.setCursor(3, INFO_Y);
    tft.print(info);
    tft.setTextColor(COL_GREY);
    tft.setCursor(3, TEXT_TOP);
    tft.print("Not shown. Load seeds via Load seed.");
    return;
  }

  char info[48];
  snprintf(info, sizeof info, "#%lu  %s  %u bytes  crc %08lx", (unsigned long)count,
           camlinkPayloadWasBase64() ? "QB" : "QR", (unsigned)len, (unsigned long)camlinkPayloadCrc());
  tft.setTextSize(1);
  tft.setTextColor(COL_BTC_ORANGE);
  tft.setCursor(3, INFO_Y);
  tft.print(info);

  const size_t shown = min(len, (size_t)(TEXT_COLS * TEXT_LINES - TEXT_COLS)); // keep a line for "more"
  tft.setTextColor(COL_WHITE);
  for (size_t i = 0; i < shown; i++) {
    if (i % TEXT_COLS == 0) tft.setCursor(3, TEXT_TOP + (int)(i / TEXT_COLS) * 10);
    uint8_t c = p[i];
    bool printable = c >= 0x20 && c <= 0x7E;
    tft.setTextColor(printable ? COL_WHITE : COL_DIM);
    tft.print(printable ? (char)c : '.');
  }
  if (shown < len) {
    char more[32];
    snprintf(more, sizeof more, "... %u more bytes", (unsigned)(len - shown));
    tft.setTextColor(COL_GREY);
    tft.setCursor(3, TEXT_TOP + (int)((shown + TEXT_COLS - 1) / TEXT_COLS) * 10);
    tft.print(more);
  }
}

static void drawStats() {
  const CamLinkStats &s = camlinkStats();
  char l1[48], l2[48], l3[48];
  snprintf(l1, sizeof l1, "ok %lu  crc fail %lu  oversize %lu", (unsigned long)s.ok,
           (unsigned long)s.crcFail, (unsigned long)s.oversize);
  snprintf(l2, sizeof l2, "malformed %lu  skip %lu  ovf %lu/%lu", (unsigned long)s.malformed,
           (unsigned long)s.skipped, (unsigned long)s.rxOverflow, (unsigned long)s.rxFifoOverflow);
  snprintf(l3, sizeof l3, "heap %u  min %u  up %lus", (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getMinFreeHeap(), (unsigned long)(millis() / 1000));
  tft.fillRect(0, STATS_Y, tft.width(), 44, COL_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY);
  tft.setCursor(3, STATS_Y);
  tft.print(l1);
  tft.setCursor(3, STATS_Y + 12);
  tft.print(l2);
  tft.setCursor(3, STATS_Y + 24);
  tft.print(l3);
}

static void logStats() {
  const CamLinkStats &s = camlinkStats();
  Serial.printf("[camlink] ok %lu, crc fail %lu, oversize %lu, malformed %lu, skipped %lu, "
                "rx overflow %lu (buffer) %lu (fifo); heap %u, min %u; max redraw %lu ms\n",
                (unsigned long)s.ok, (unsigned long)s.crcFail, (unsigned long)s.oversize,
                (unsigned long)s.malformed, (unsigned long)s.skipped, (unsigned long)s.rxOverflow,
                (unsigned long)s.rxFifoOverflow, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)maxDrawMs);
}

void scanTestScreen() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Scan test", 4, 2, COL_BTC_ORANGE);
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY);
  tft.setCursor(3, TEXT_TOP);
  tft.print("Waiting for the camera...");
  drawButton(backButton, false);

  camlinkReset();
  maxDrawMs = 0;
  drawStats();
  uint32_t count = 0, lastStatsMs = millis(), lastLogMs = millis(), lastDrawMs = 0;
  bool backHeld = false, drawPending = false;
  while (true) {
    while (camlinkPoll()) {
      count++;
      drawPending = true;
      // Length and CRC only: payloads are never printed. Not even the CRC
      // for SeedQR-shaped payloads (a CRC-32 reveals 32 bits of a seed).
      if (seedqrLooksLike(camlinkPayload(), camlinkPayloadLen())) {
        Serial.printf("scan #%lu: %u bytes, looks like a SeedQR (not logged)\n", (unsigned long)count,
                      (unsigned)camlinkPayloadLen());
      } else {
        Serial.printf("scan #%lu: %s %u bytes crc %08lx\n", (unsigned long)count,
                      camlinkPayloadWasBase64() ? "QB" : "QR", (unsigned)camlinkPayloadLen(),
                      (unsigned long)camlinkPayloadCrc());
      }
    }
    uint32_t now = millis();
    if (drawPending && now - lastDrawMs >= REDRAW_MS) {
      drawPayload(count);
      drawStats();
      drawPending = false;
      lastDrawMs = millis();
      maxDrawMs = max(maxDrawMs, lastDrawMs - now);
      now = lastDrawMs;
    }
    if (now - lastStatsMs >= 1000) { drawStats(); lastStatsMs = now; }
    if (now - lastLogMs >= 10000) { logStats(); lastLogMs = now; }

    TouchEvent e = touchPoll();
    if (e.type == TOUCH_PRESS && buttonHit(backButton, e.x, e.y)) {
      backHeld = true;
      drawButton(backButton, true);
    } else if (e.type == TOUCH_RELEASE && backHeld) {
      logStats();
      camlinkWipe(); // test screens leave nothing scanned behind
      return;
    }
    delay(5);
  }
}
