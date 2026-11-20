#include "address_screen.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include "display.h"
#include "keys.h"
#include "qr_out.h"
#include "secure_mem.h"
#include "session.h"
#include "ui.h"

#define MAX_INDEX 9999 // matches the signer's sane change-index limit (<= 10000)

static int progressPct = -1;
static void progressBar(float f) {
  int pct = (int)(f * 100);
  if (pct / 5 == progressPct / 5) return;
  progressPct = pct;
  tft.fillRect(20, 200, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

// "tb1q6rz28mcf..." -> "tb1q 6rz2 8mcf ..."
static void grouped(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < cap; i++) {
    if (i && i % 4 == 0) out[o++] = ' ';
    out[o++] = in[i];
  }
  out[o] = 0;
}

static void drawAddress(int chain, int index, const char *addr) {
  tft.fillScreen(COL_BLACK);
  char title[24], path[32];
  snprintf(title, sizeof title, "%s #%d", chain == 0 ? "Receive" : "Change", index);
  snprintf(path, sizeof path, "m/84'/1'/0'/%d/%d", chain, index);
  drawCentredText(title, 4, 2, chain == 0 ? COL_WHITE : COL_BTC_ORANGE);
  drawCentredText(path, 26, 1, COL_GREY);

  // 4 groups of 4 per line at size 2 (19 chars = 228 px): 3 lines for tb1q (42 chars).
  char g[64];
  grouped(addr, g, sizeof g);
  tft.setTextSize(2);
  tft.setTextColor(COL_WHITE, COL_BLACK);
  const int perLine = 20;
  int y = 42;
  for (size_t off = 0; off < strlen(g); off += perLine, y += 20) {
    char line[24];
    size_t n = strlen(g + off);
    if (n > (size_t)perLine) n = perLine;
    memcpy(line, g + off, n);
    while (n && line[n - 1] == ' ') n--;
    line[n] = 0;
    tft.setCursor(6, y);
    tft.print(line);
  }

  // QR of the address in upper case: bech32 allows it (BIP173), and it fits
  // QR's alphanumeric mode, so the code is smaller with bigger modules.
  char upper[80];
  size_t n = strlen(addr);
  for (size_t i = 0; i <= n && i < sizeof upper; i++) upper[i] = (char)toupper((unsigned char)addr[i]);
  upper[sizeof upper - 1] = 0;
  qrDraw((const uint8_t *)upper, strlen(upper), 40, 104, 160, 160, QR_ECC_LOW);
}

void addressScreen() {
  if (!session.loaded) return;
  tft.fillScreen(COL_BLACK);
  drawCentredText("Addresses", 20, 2, COL_BTC_ORANGE);
  drawCentredText("Deriving the account...", 150, 1, COL_WHITE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;

  AccountInfo *a = (AccountInfo *)calloc(1, sizeof(AccountInfo));
  if (!a) return;
  keysAccount(session.mnemonic.c_str(), session.mnemonic.size(), session.passphrase.c_str(),
              session.passphrase.size(), *a, progressBar);
  secure_scrub_stack();

  // From here on only public derivation, from the account tpub. Check that
  // parsing it gives the same receive #0 as the full derivation did.
  HDPublicKey account(a->tpub);
  char addr[80];
  account.child(0).child(0).address(addr, sizeof addr);
  bool selfOk = strcmp(addr, a->receive[0]) == 0;
  Serial.printf("[addr] account tpub parsed; receive #0 self-check %s\n", selfOk ? "OK" : "FAIL");
  free(a);
  if (!selfOk) {
    tft.fillScreen(COL_BLACK);
    drawCentredText("Error", 40, 3, COL_RED);
    drawTextWrapped("Address self-check failed. Don't use these addresses.", 16, 100, 208, 2, COL_WHITE);
    const Button ok = {70, 262, 100, 46, "OK", COL_GREY, COL_BLACK, 2};
    drawButton(ok, false);
    runButtons(&ok, 1);
    return;
  }

  int chain = 0, index = 0;
  const Button b[5] = {
      {2, 272, 44, 44, "-1", COL_GREY, COL_BLACK, 2},
      {48, 272, 44, 44, "+1", COL_GREY, COL_BLACK, 2},
      {94, 272, 50, 44, "+10", COL_GREY, COL_BLACK, 2},
      {146, 272, 46, 44, "Chg", COL_BTC_ORANGE, COL_BLACK, 2},
      {194, 272, 44, 44, "OK", COL_GREEN, COL_BLACK, 2},
  };
  while (true) {
    HDPublicKey chainKey = account.child(chain);
    chainKey.child(index).address(addr, sizeof addr);
    drawAddress(chain, index, addr);
    Serial.printf("[addr] %s #%d m/84'/1'/0'/%d/%d %s\n", chain == 0 ? "receive" : "change", index, chain, index,
                  addr);
    Button shown[5];
    memcpy(shown, b, sizeof shown);
    shown[3].label = chain == 0 ? "Chg" : "Rcv";
    for (const Button &k : shown) drawButton(k, false);
    int k = runButtons(shown, 5);
    if (k == 0) index = index > 0 ? index - 1 : 0;
    else if (k == 1) index = index < MAX_INDEX ? index + 1 : MAX_INDEX;
    else if (k == 2) index = index + 10 <= MAX_INDEX ? index + 10 : MAX_INDEX;
    else if (k == 3) {
      chain ^= 1;
      index = 0;
    } else break;
  }
}
