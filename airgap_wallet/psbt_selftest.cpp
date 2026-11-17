#include "psbt_selftest.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "display.h"
#include "psbt_flow.h"
#include "psbt_review.h"
#include "ui.h"
#include "psbt_selftest_vectors.h"

static int failures;

// Compares a verdict with what the fixture expects. Empty string = as expected.
static void compare(const PtCase &c, const Review &r, char *why, size_t cap) {
  why[0] = 0;
  if (r.ok != c.ok) {
    snprintf(why, cap, "verdict: got %s (%s)", r.ok ? "accepted" : "refused", r.reason);
    return;
  }
  if (!c.ok) {
    if (!strstr(r.reason, c.reason)) snprintf(why, cap, "reason: \"%s\"", r.reason);
    return;
  }
  if (r.fee != c.fee || r.sentTotal != c.sent || r.changeTotal != c.change) {
    snprintf(why, cap, "amounts: fee %llu sent %llu change %llu", (unsigned long long)r.fee,
             (unsigned long long)r.sentTotal, (unsigned long long)r.changeTotal);
    return;
  }
  if (r.amountsVerified != c.verified) {
    snprintf(why, cap, "amountsVerified %d", r.amountsVerified);
    return;
  }
  if (r.highFee != (c.high[0] != 0) || (r.highFee && strcmp(r.highFeeWhy, c.high) != 0)) {
    snprintf(why, cap, "high fee: %d \"%s\"", r.highFee, r.highFeeWhy);
    return;
  }
  if (r.outputCount != c.outCount) {
    snprintf(why, cap, "%lu outputs", (unsigned long)r.outputCount);
    return;
  }
  for (uint32_t i = 0; i < c.outCount; i++) {
    const ReviewOutput &o = r.outputs[i];
    const PtOutput &w = c.outs[i];
    if (o.kind != w.kind || o.index != w.index || o.claimFailed != w.claim || o.amount != w.amount ||
        strcmp(o.address, w.address) != 0) {
      snprintf(why, cap, "output %lu: kind %d index %lu claim %d amount %llu %s", (unsigned long)i, o.kind,
               (unsigned long)o.index, o.claimFailed, (unsigned long long)o.amount, o.address);
      return;
    }
  }
}

void psbtSelfTestScreen() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("PSBT self-test", 12, 2, COL_WHITE);
  drawCentredText("Running...", 120, 2, COL_GREY);
  tft.drawRect(19, 149, tft.width() - 38, 14, COL_GREY);
  failures = 0;
  const size_t count = sizeof PT_CASES / sizeof *PT_CASES;
  Serial.printf("[psbt] self-test: %u fixtures\n", (unsigned)count);

  PsbtInfo *info = (PsbtInfo *)malloc(sizeof(PsbtInfo));
  Review *r = (Review *)malloc(sizeof(Review));
  if (!info || !r) {
    free(info); free(r);
    messageScreen("PSBT self-test", "Out of memory.");
    return;
  }
  HDPublicKey account(PT_TPUB);
  size_t heap[3];
  uint32_t t0 = millis(), maxMs = 0, minHeap = UINT32_MAX;
  heap[0] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  // Two runs, as in the UR self-test: a leak would show up as less free heap
  // after the second run than after the first.
  for (int run = 0; run < 2; run++) {
    for (size_t i = 0; i < count; i++) {
      const PtCase &c = PT_CASES[i];
      uint32_t ts = millis();
      psbtCheck(c.data, c.len, account, PT_FP, *info, *r);
      uint32_t ms = millis() - ts;
      if (ms > maxMs) maxMs = ms;
      size_t h = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      if (h < minHeap) minHeap = h;
      char why[140];
      compare(c, *r, why, sizeof why);
      if (why[0]) failures++;
      if (run == 0 || why[0]) {
        Serial.printf("  %-58s %s %s\n", c.name, why[0] ? "FAIL" : "OK", why);
      }
      if (run == 0) {
        tft.fillRect(20, 150, (tft.width() - 40) * (i + 1) / count, 12, COL_BTC_ORANGE);
      }
      yield();
    }
    heap[run + 1] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  }
  free(info);
  free(r);
  bool leak = heap[2] != heap[1];
  if (leak) failures++;
  Serial.printf("  no heap leak (run 2 frees all it allocates): %s (free: %u at start, %u after run 1, %u after run 2)\n",
                leak ? "FAIL" : "OK", (unsigned)heap[0], (unsigned)heap[1], (unsigned)heap[2]);
  bool ok = failures == 0;
  Serial.printf("[psbt] self-test %s (%d failed of %u; %lu ms for two runs, slowest case %lu ms; lowest free heap ever %lu B; "
                "loop stack %u B free at the low point)\n",
                ok ? "PASS" : "FAIL", failures, (unsigned)(count * 2), (unsigned long)(millis() - t0),
                (unsigned long)maxMs, (unsigned long)minHeap, (unsigned)uxTaskGetStackHighWaterMark(NULL));

  tft.fillRect(0, 110, tft.width(), 60, COL_BLACK);
  drawCentredText(ok ? "PASS" : "FAIL", 130, 3, ok ? COL_GREEN : COL_RED);
  char line[48];
  snprintf(line, sizeof line, "%d of %u failed", failures, (unsigned)(count * 2));
  drawCentredText(ok ? "All fixtures as expected" : line, 175, 1, COL_GREY);
  drawCentredText("Details on the serial log", 190, 1, COL_GREY);
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);
  runButtons(&back, 1);
}
