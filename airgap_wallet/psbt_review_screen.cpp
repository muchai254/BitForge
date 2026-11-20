#include "psbt_review_screen.h"
#include <Arduino.h>
#include "display.h"
#include "ui.h"

static const Button rejectBtn = {8, 268, 108, 46, "Reject", COL_RED, COL_WHITE, 2};
static const Button nextBtn = {124, 268, 108, 46, "Next", COL_GREY, COL_BLACK, 2};
static const Button approveBtn = {124, 268, 108, 46, "Approve", COL_GREEN, COL_BLACK, 2};

void psbtRefusedScreen(const char *reason) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Refused", 16, 3, COL_RED);
  int y = drawTextWrapped(reason, 12, 60, 216, 2, COL_WHITE);
  drawTextWrapped("Nothing was signed.", 12, y + 12, 216, 2, COL_GREY);
  const Button ok = {70, 262, 100, 46, "OK", COL_GREY, COL_BLACK, 2};
  drawButton(ok, false);
  runButtons(&ok, 1);
}

// The full address in groups of 4, four groups per line (as on the address screen).
static int drawAddressGrouped(const char *addr, int y, uint16_t color) {
  char g[REVIEW_ADDR_LEN * 5 / 4 + 4];
  size_t o = 0;
  for (size_t i = 0; addr[i] && o + 2 < sizeof g; i++) {
    if (i && i % 4 == 0) g[o++] = ' ';
    g[o++] = addr[i];
  }
  g[o] = 0;
  tft.setTextSize(2);
  tft.setTextColor(color, COL_BLACK);
  const size_t perLine = 20;
  for (size_t off = 0; off < o; off += perLine, y += 20) {
    char line[24];
    size_t n = o - off < perLine ? o - off : perLine;
    memcpy(line, g + off, n);
    while (n && line[n - 1] == ' ') n--;
    line[n] = 0;
    tft.setCursor(6, y);
    tft.print(line);
  }
  return y;
}

// Returns true for Next.
static bool recipientScreen(const Review &r, const ReviewOutput &o, int k, int total) {
  tft.fillScreen(COL_BLACK);
  char title[32];
  snprintf(title, sizeof title, "%s %d of %d", o.kind == OUT_SELF ? "To self" : "Send", k, total);
  drawCentredText(title, 6, 2, o.kind == OUT_SELF ? COL_GREEN : COL_BTC_ORANGE);
  char btc[32], sats[32], line[48];
  formatBtc(o.amount, btc, sizeof btc);
  formatSats(o.amount, sats, sizeof sats);
  snprintf(line, sizeof line, "%s BTC", btc);
  drawCentredText(line, 34, 2, COL_WHITE);
  snprintf(line, sizeof line, "%s sats", sats);
  drawCentredText(line, 56, 1, COL_GREY);
  drawCentredText("to", 74, 1, COL_GREY);
  int y = drawAddressGrouped(o.address, 88, COL_WHITE) + 8;
  if (o.kind == OUT_SELF) {
    snprintf(line, sizeof line, "Your own receive address #%lu (verified).", (unsigned long)o.index);
    drawTextWrapped(line, 12, y, 216, 1, COL_GREEN);
  } else if (o.claimFailed) {
    drawTextWrapped("WARNING: the PSBT says this output is yours, but it isn't. Treat it as a payment to someone else.",
                    12, y, 216, 1, COL_RED);
  } else {
    drawTextWrapped("Check this address against the one you meant to pay, character by character.", 12, y, 216, 1,
                    COL_GREY);
  }
  drawButton(rejectBtn, false);
  drawButton(nextBtn, false);
  const Button b[2] = {rejectBtn, nextBtn};
  return runButtons(b, 2) == 1;
}

static int summaryLine(const char *label, const char *value, int y, uint16_t color = COL_WHITE) {
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY, COL_BLACK);
  tft.setCursor(12, y);
  tft.print(label);
  tft.setTextColor(color, COL_BLACK);
  tft.setCursor(84, y);
  tft.print(value);
  return y + 14;
}

// Returns true for Approve.
static bool summaryScreen(const Review &r, int recipients) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Summary", 6, 2, COL_BTC_ORANGE);
  char a[32], b[32], v[64];
  int y = 36;
  formatBtc(r.sentTotal, a, sizeof a);
  snprintf(v, sizeof v, "%s BTC", a);
  y = summaryLine("Sending", v, y);
  formatSats(r.sentTotal, a, sizeof a);
  snprintf(v, sizeof v, "%s sats, %d output%s", a, recipients, recipients == 1 ? "" : "s");
  y = summaryLine("", v, y, COL_GREY);
  formatSats(r.fee, a, sizeof a);
  snprintf(v, sizeof v, "%s sats", a);
  y = summaryLine("Fee", v, y, r.highFee ? COL_BTC_ORANGE : COL_WHITE);
  snprintf(v, sizeof v, "%lu.%lu sat/vB (~%lu vB)", (unsigned long)(r.feeRateX10 / 10), (unsigned long)(r.feeRateX10 % 10),
           (unsigned long)r.vsize);
  y = summaryLine("", v, y, COL_GREY);
  int changes = 0;
  for (uint32_t i = 0; i < r.outputCount; i++) {
    const ReviewOutput &o = r.outputs[i];
    if (o.kind != OUT_CHANGE) continue;
    formatSats(o.amount, a, sizeof a);
    snprintf(v, sizeof v, "%s sats (#%lu)", a, (unsigned long)o.index);
    y = summaryLine(changes++ ? "" : "Change", v, y);
  }
  if (!changes) y = summaryLine("Change", "none", y, COL_GREY);
  formatSats(r.inputTotal, a, sizeof a);
  snprintf(v, sizeof v, "%lu, total %s sats", (unsigned long)r.inputCount, a);
  y = summaryLine("Inputs", v, y);
  snprintf(b, sizeof b, "%lu", (unsigned long)r.locktime);
  snprintf(v, sizeof v, "%s, RBF %s", b, r.rbf ? "on" : "off");
  y = summaryLine("Locktime", v, y, COL_GREY);
  y += 4;
  if (!r.amountsVerified) {
    y = drawTextWrapped("Fee not independently verified (no previous transaction). A wrong amount would only make "
                        "the signature invalid.",
                        12, y, 216, 1, COL_BTC_ORANGE) + 4;
  }
  if (r.highFee) {
    snprintf(v, sizeof v, "High fee: %s.", r.highFeeWhy);
    drawTextWrapped(v, 12, y, 216, 1, COL_BTC_ORANGE);
  }
  drawButton(rejectBtn, false);
  drawButton(approveBtn, false);
  const Button bs[2] = {rejectBtn, approveBtn};
  return runButtons(bs, 2) == 1;
}

bool psbtReviewScreens(const Review &r) {
  int recipients = 0;
  for (uint32_t i = 0; i < r.outputCount; i++)
    if (r.outputs[i].kind != OUT_CHANGE) recipients++;
  int k = 0;
  for (uint32_t i = 0; i < r.outputCount; i++) {
    const ReviewOutput &o = r.outputs[i];
    if (o.kind == OUT_CHANGE) continue;
    if (!recipientScreen(r, o, ++k, recipients)) return false;
  }
  if (!summaryScreen(r, recipients)) return false;
  if (r.highFee) {
    char a[32], msg[128];
    formatSats(r.fee, a, sizeof a);
    snprintf(msg, sizeof msg, "The fee is %s sats, %s. Approve it anyway?", a, r.highFeeWhy);
    if (!confirmScreen("High fee", msg)) return false;
  }
  return confirmScreen("Sign?", "Sign this transaction with the loaded seed?");
}
