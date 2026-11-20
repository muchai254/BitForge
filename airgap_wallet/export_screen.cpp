#include "export_screen.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include "camlink.h"
#include "descriptor.h"
#include "display.h"
#include "keys.h"
#include "qr_out.h"
#include "secure_mem.h"
#include "seedqr.h"
#include "session.h"
#include "touch.h"
#include "ui.h"

#define QR_TOP 20
#define QR_BOX 240

static int progressPct = -1;
static void progressBar(float f) {
  int pct = (int)(f * 100);
  if (pct / 5 == progressPct / 5) return;
  progressPct = pct;
  tft.fillRect(20, 200, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

// "tb1q6rz28mcf..." -> "tb1q 6rz2 8mcf ..." (easier to compare by eye).
static void grouped(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < cap; i++) {
    if (i && i % 4 == 0) out[o++] = ' ';
    out[o++] = in[i];
  }
  out[o] = 0;
}

static void addressesScreen(const AccountInfo &a) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Addresses", 6, 2, COL_BTC_ORANGE);
  int y = 30;
  char g[80], label[24];
  for (int i = 0; i < KEYS_ADDRESSES + 2; i++) {
    bool change = i >= KEYS_ADDRESSES;
    int idx = change ? i - KEYS_ADDRESSES : i;
    snprintf(label, sizeof label, "%s #%d", change ? "change" : "receive", idx);
    grouped(change ? a.change[idx] : a.receive[idx], g, sizeof g);
    y = drawTextWrapped(label, 6, y, 228, 1, change ? COL_BTC_ORANGE : COL_GREY);
    y = drawTextWrapped(g, 6, y, 228, 1, COL_WHITE) + 5;
  }
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);
  runButtons(&back, 1);
}

void exportScreen() {
  if (!session.loaded) return;
  tft.fillScreen(COL_BLACK);
  drawCentredText("Export to Sparrow", 20, 2, COL_BTC_ORANGE);
  drawCentredText("Deriving the account...", 150, 1, COL_WHITE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;

  AccountInfo *a = (AccountInfo *)calloc(1, sizeof(AccountInfo));
  char *desc = (char *)calloc(1, 220);
  char *keyOrigin = (char *)calloc(1, 180);
  if (!a || !desc || !keyOrigin) {
    free(a);
    free(desc);
    free(keyOrigin);
    return;
  }
  keysAccount(session.mnemonic.c_str(), session.mnemonic.size(), session.passphrase.c_str(),
              session.passphrase.size(), *a, progressBar);
  secure_scrub_stack();
  bool okDesc = descriptorWpkhAccount(a->fingerprint, a->tpub, desc, 220);
  bool okKey = keyWithOrigin(a->fingerprint, a->tpub, keyOrigin, 180);
  // Public data only (xpubs and addresses may be shown).
  Serial.printf("[export] %s\n", okDesc ? desc : "(descriptor failed)");
  Serial.printf("[export] receive 0..2: %s %s %s\n", a->receive[0], a->receive[1], a->receive[2]);

  int format = 0; // 0 = descriptor, 1 = key with origin
  while (true) {
    const char *data = format == 0 ? desc : keyOrigin;
    bool ok = format == 0 ? okDesc : okKey;
    tft.fillScreen(COL_BLACK);
    drawCentredText(format == 0 ? "Descriptor" : "Key with origin", 2, 2, COL_BTC_ORANGE);
    QrInfo q = {0, 0, 0};
    if (ok) q = qrDraw((const uint8_t *)data, strlen(data), 0, QR_TOP, QR_BOX, QR_BOX, QR_ECC_LOW);
    char cap[64];
    if (q.version) snprintf(cap, sizeof cap, "QR v%d, %d px/module, %u chars", q.version, q.modulePx, (unsigned)strlen(data));
    else snprintf(cap, sizeof cap, "Couldn't make the QR");
    drawCentredText(cap, QR_TOP + QR_BOX + 2, 1, q.version ? COL_GREY : COL_RED);
    Serial.printf("[export] showing %s: %s\n", format == 0 ? "descriptor" : "key with origin", cap);

    const Button b[3] = {
        {4, 274, 76, 42, "Format", COL_GREY, COL_BLACK, 2},
        {84, 274, 84, 42, "Addrs", COL_GREY, COL_BLACK, 2},
        {172, 274, 64, 42, "Done", COL_BTC_ORANGE, COL_BLACK, 2},
    };
    for (const Button &k : b) drawButton(k, false);
    int k = runButtons(b, 3);
    if (k == 0) format ^= 1;
    else if (k == 1) addressesScreen(*a);
    else break;
  }
  free(a);
  free(desc);
  free(keyOrigin);
}

void qrLoopbackTestScreen() {
  bool selfOk = qrSelfTest();
  // Short payload -> version 2 (25 modules) -> 7 px/module across the full
  // width, so the camera can read it from further away (its fixed-focus lens
  // is blurry up close).
  static const char TEST_PAYLOAD[] = "airgap loopback 0123456789";

  tft.fillScreen(COL_BLACK);
  QrInfo q = qrDraw((const uint8_t *)TEST_PAYLOAD, strlen(TEST_PAYLOAD), 0, 0, 240, 232, QR_ECC_LOW);
  char line[64];
  snprintf(line, sizeof line, "encoder self-test %s, QR v%d %dpx", selfOk ? "PASS" : "FAIL", q.version, q.modulePx);
  drawCentredText(line, 236, 1, selfOk ? COL_GREY : COL_RED);
  drawCentredText("Point the camera at this QR", 248, 1, COL_WHITE);
  Serial.printf("[qr] loopback QR: %s\n", line);
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);

  camlinkReset();
  int good = 0, bad = 0;
  bool backHeld = false;
  while (true) {
    if (camlinkPoll()) {
      bool same = camlinkPayloadLen() == strlen(TEST_PAYLOAD) &&
                  memcmp(camlinkPayload(), TEST_PAYLOAD, camlinkPayloadLen()) == 0;
      if (same) good++;
      else bad++;
      if (!same && seedqrLooksLike(camlinkPayload(), camlinkPayloadLen())) {
        Serial.printf("[qr] loopback read: %u bytes, looks like a SeedQR (not logged)\n",
                      (unsigned)camlinkPayloadLen());
      } else {
        Serial.printf("[qr] loopback read: %u bytes crc %08lx, %s\n", (unsigned)camlinkPayloadLen(),
                      (unsigned long)camlinkPayloadCrc(), same ? "MATCH" : "different");
      }
      snprintf(line, sizeof line, "read back OK %d, other %d", good, bad);
      tft.fillRect(0, 258, 146, 16, COL_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(good ? COL_GREEN : COL_RED);
      tft.setCursor(4, 262);
      tft.print(line);
    }
    TouchEvent e = touchPoll();
    if (e.type == TOUCH_PRESS && buttonHit(back, e.x, e.y)) {
      backHeld = true;
      drawButton(back, true);
    } else if (e.type == TOUCH_RELEASE && backHeld) {
      camlinkWipe(); // test screens leave nothing scanned behind
      return;
    }
    delay(5);
  }
}
