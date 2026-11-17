#include "ur_selftest.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include "display.h"
#include "ui.h"
#include "src/ur/ur.h"
#include "ur_selftest_vectors.h"

static int failures;
static bool quiet; // second run: only failures are logged

static bool check(const char *label, bool ok, const char *detail = "") {
  if (!quiet || !ok) Serial.printf("  %-44s %s %s\n", label, ok ? "OK" : "FAIL", detail);
  if (!ok) failures++;
  return ok;
}

// make_message(len, seed) from bc-ur's tests: Xoshiro256(seed) bytes.
static void makeMessage(uint8_t *out, size_t len, const char *seed) {
  ur::Xoshiro256 rng((const uint8_t *)seed, strlen(seed));
  for (size_t i = 0; i < len; i++) out[i] = (uint8_t)rng.nextInt(0, 255);
}

static void testPrimitives() {
  check("crc32 \"Hello, world!\"", ur::crc32((const uint8_t *)"Hello, world!", 13) == 0xebe6c6e6);
  check("crc32 \"Wolf\"", ur::crc32((const uint8_t *)"Wolf", 4) == 0x598c84dc);

  uint8_t buf[128];
  int n = ur::bytewordsDecodeMinimal(UT_BW1_MINIMAL, strlen(UT_BW1_MINIMAL), buf, sizeof buf);
  check("bytewords 1 minimal decode", n == (int)sizeof UT_BW1_INPUT && memcmp(buf, UT_BW1_INPUT, n) == 0);
  check("bytewords 1 bad checksum refused", ur::bytewordsDecodeMinimal(UT_BW1_BAD, strlen(UT_BW1_BAD), buf, sizeof buf) < 0);
  n = ur::bytewordsDecodeMinimal(UT_BW2_MINIMAL, strlen(UT_BW2_MINIMAL), buf, sizeof buf);
  check("bytewords 2 minimal decode (100 bytes)", n == (int)sizeof UT_BW2_INPUT && memcmp(buf, UT_BW2_INPUT, n) == 0);
  check("bytewords too short refused", ur::bytewordsDecodeMinimal("aeadao", 6, buf, sizeof buf) < 0);

  bool ok = true;
  ur::Xoshiro256 r1((const uint8_t *)"Wolf", 4);
  for (int i = 0; i < 100; i++) ok = ok && r1.next() % 100 == UT_RNG1[i];
  check("xoshiro256 \"Wolf\" (100 values)", ok);
  uint32_t c = ur::crc32((const uint8_t *)"Wolf", 4);
  uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
  ur::Xoshiro256 r2(cb, 4);
  ok = true;
  for (int i = 0; i < 100; i++) ok = ok && r2.next() % 100 == UT_RNG2[i];
  check("xoshiro256 crc32(\"Wolf\")", ok);
  ur::Xoshiro256 r3((const uint8_t *)"Wolf", 4);
  ok = true;
  for (int i = 0; i < 100; i++) ok = ok && r3.nextInt(1, 10) == UT_RNG3[i];
  check("xoshiro256 next_int(1, 10)", ok);

  ur::DegreeSampler sampler;
  ok = sampler.init(UT_PARTITION_SEQLEN);
  for (int i = 0; ok && i < 200; i++) {
    char seed[16];
    snprintf(seed, sizeof seed, "Wolf-%d", i + 1);
    ur::Xoshiro256 rng((const uint8_t *)seed, strlen(seed));
    ok = sampler.next(rng) + 1 == UT_DEGREES[i];
  }
  check("choose_degree (200)", ok);
  uint8_t bits[2];
  uint32_t scratch[UT_PARTITION_SEQLEN];
  ok = true;
  for (uint32_t s = 1; s <= 30; s++) {
    ur::chooseFragments(s, UT_PARTITION_SEQLEN, UT_MSG1024_CRC, sampler, bits, scratch);
    ok = ok && (uint16_t)(bits[0] | bits[1] << 8) == UT_FRAGMENTS[s - 1];
  }
  check("choose_fragments (30)", ok);
}

// Feeds parts in order; returns the number fed when complete (0 if never).
static uint32_t feed(ur::Decoder &d, const char *const *parts, size_t count) {
  for (size_t i = 0; i < count; i++) {
    ur::Status st = d.receive(parts[i], strlen(parts[i]));
    if (st == ur::COMPLETE) return (uint32_t)(i + 1);
    if (st == ur::FAILED) return 0;
  }
  return 0;
}

static bool messageIs(ur::Decoder &d, size_t len, const char *seed) {
  const uint8_t *body;
  size_t n;
  if (!d.isComplete() || !ur::unwrapByteString(d.message(), d.messageLen(), &body, &n) || n != len) return false;
  uint8_t *want = (uint8_t *)malloc(len);
  if (!want) return false;
  makeMessage(want, len, seed);
  bool ok = memcmp(body, want, len) == 0;
  free(want);
  return ok;
}

static void testDecoder() {
  char detail[64];
  {
    ur::Decoder d;
    uint32_t used = feed(d, UT_UR_PARTS, sizeof UT_UR_PARTS / sizeof *UT_UR_PARTS);
    snprintf(detail, sizeof detail, "(complete after %lu of 20)", (unsigned long)used);
    check("upstream multipart UR (make_message(256))", used && messageIs(d, 256, "Wolf"), detail);
  }
  {
    ur::Decoder d;
    check("upstream single-part UR (make_message(50))",
          d.receive(UT_SINGLE_PART, strlen(UT_SINGLE_PART)) == ur::COMPLETE && messageIs(d, 50, "Wolf"));
  }
  {
    ur::Decoder d;
    uint32_t t0 = millis();
    size_t count = sizeof UT_LOSSY / sizeof *UT_LOSSY;
    uint32_t used = feed(d, UT_LOSSY, count);
    snprintf(detail, sizeof detail, "(%lu of %u strings, ur.js needs %lu distinct; %lu ms)", (unsigned long)used,
             (unsigned)count, (unsigned long)UT_LOSSY_JS_NEEDED, (unsigned long)(millis() - t0));
    check("lossy stream: 3000 B, 50 fragments, all mixed", used && messageIs(d, 3000, "Loss"), detail);
  }
}

// The encoder must produce bc-ur's exact strings, and its output (from the
// mixed parts onward, and a single part) must decode back.
static void testEncoder() {
  uint8_t msg[256], cbor[261];
  makeMessage(msg, sizeof msg, "Wolf");
  size_t cborLen = ur::wrapByteString(msg, sizeof msg, cbor);
  char text[256];
  {
    ur::Encoder e;
    bool ok = e.begin("bytes", cbor, cborLen, 30);
    size_t count = sizeof UT_UR_PARTS / sizeof *UT_UR_PARTS;
    for (size_t i = 0; ok && i < count; i++) ok = e.nextPart(text, sizeof text) > 0 && strcmp(text, UT_UR_PARTS[i]) == 0;
    check("encoder: upstream multipart strings (20)", ok);
  }
  {
    ur::Encoder e;
    ur::Decoder d;
    bool ok = e.begin("bytes", cbor, cborLen, 30, 10, 1000); // starts deep in the mixed parts
    uint32_t parts = 0;
    while (ok && !d.isComplete() && parts < 200) {
      int n = e.nextPart(text, sizeof text);
      ok = n > 0;
      if (ok) d.receive(text, n);
      parts++;
    }
    char detail[32];
    snprintf(detail, sizeof detail, "(%lu parts)", (unsigned long)parts);
    check("encoder -> decoder, mixed parts only", ok && messageIs(d, sizeof msg, "Wolf"), detail);
  }
  {
    uint8_t m50[50], c50[55];
    makeMessage(m50, sizeof m50, "Wolf");
    size_t n = ur::wrapByteString(m50, sizeof m50, c50);
    ur::Encoder e;
    bool ok = e.begin("bytes", c50, n, 100) && e.nextPart(text, sizeof text) > 0;
    check("encoder: upstream single-part string", ok && strcmp(text, UT_SINGLE_PART) == 0);
  }
  check("encoder: output buffer too small refused", [&] {
    ur::Encoder e;
    char tiny[16];
    return e.begin("bytes", cbor, cborLen, 30) && e.nextPart(tiny, sizeof tiny) < 0;
  }());
}

static void testHostile() {
  static const char *const types[] = {"crypto-psbt", "psbt", nullptr};
  for (const UrCase &c : UT_HOSTILE) {
    ur::Decoder d(types);
    ur::Status st = d.receive(c.text, strlen(c.text));
    char detail[48];
    snprintf(detail, sizeof detail, "(%s)", ur::statusName(st));
    check(c.what, st == c.want, detail);
  }
  ur::Decoder d(types);
  check("first part of a message accepted", d.receive(UT_HOSTILE_FIRST, strlen(UT_HOSTILE_FIRST)) == ur::ACCEPTED);
  for (const UrCase &c : UT_HOSTILE_AFTER) {
    ur::Status st = d.receive(c.text, strlen(c.text));
    char detail[48];
    snprintf(detail, sizeof detail, "(%s)", ur::statusName(st));
    check(c.what, st == c.want, detail);
  }
  check("refused parts left the decoder unchanged", d.fragmentsKnown() == 1 && d.partsAccepted() == 1);
}

static void testSparrow(const char *label, const char *const *parts, size_t count) {
  static const char *const types[] = {"crypto-psbt", "psbt", nullptr};
  ur::Decoder d(types);
  uint32_t t0 = millis();
  uint32_t used = feed(d, parts, count);
  uint32_t ms = millis() - t0;
  const uint8_t *psbt;
  size_t n = 0;
  uint8_t h[32] = {0};
  bool ok = used && ur::unwrapByteString(d.message(), d.messageLen(), &psbt, &n) && n == UT_SPARROW_PSBT_LEN;
  if (ok) {
    mbedtls_sha256(psbt, n, h, 0);
    ok = memcmp(h, UT_SPARROW_PSBT_SHA256, 32) == 0 && memcmp(psbt, "psbt\xff", 5) == 0;
  }
  char detail[64];
  snprintf(detail, sizeof detail, "(%lu frames, %u B PSBT, SHA-256 %s, %lu ms)", (unsigned long)used, (unsigned)n,
           ok ? "matches" : "differs", (unsigned long)ms);
  check(label, ok, detail);
}

void urSelfTestScreen() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("UR self-test", 12, 2, COL_WHITE);
  drawCentredText("Running...", 140, 2, COL_GREY);
  failures = 0;
  uint32_t t0 = millis();
  Serial.println("[ur] self-test: bc-ur vectors, lossy stream, hostile inputs, Sparrow captures");
  // Run everything twice. The first run makes one-time allocations (e.g. the
  // SHA hardware driver's lock, 108 B); a leak would show up as less free
  // heap after the second run than after the first.
  size_t heap[3];
  heap[0] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  for (int run = 0; run < 2; run++) {
    quiet = run == 1;
    testPrimitives();
    testDecoder();
    testEncoder();
    testHostile();
    testSparrow("Sparrow capture, 2 fragments (651-char QRs)", UT_SPARROW_2, sizeof UT_SPARROW_2 / sizeof *UT_SPARROW_2);
    testSparrow("Sparrow capture, 8 fragments (204-char QRs)", UT_SPARROW_8, sizeof UT_SPARROW_8 / sizeof *UT_SPARROW_8);
    heap[run + 1] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  }
  quiet = false;
  char detail[64];
  snprintf(detail, sizeof detail, "(free: %u at start, %u after run 1, %u after run 2)", (unsigned)heap[0],
           (unsigned)heap[1], (unsigned)heap[2]);
  check("no heap leak (run 2 frees all it allocates)", heap[2] == heap[1], detail);
  bool ok = failures == 0;
  Serial.printf("[ur] self-test %s (%d failed, %lu ms; loop stack %u B free at the low point)\n", ok ? "PASS" : "FAIL",
                failures, (unsigned long)(millis() - t0), (unsigned)uxTaskGetStackHighWaterMark(NULL));

  tft.fillRect(0, 120, tft.width(), 50, COL_BLACK);
  drawCentredText(ok ? "PASS" : "FAIL", 130, 3, ok ? COL_GREEN : COL_RED);
  char line[48];
  snprintf(line, sizeof line, "%d check%s failed", failures, failures == 1 ? "" : "s");
  drawCentredText(ok ? "All checks passed" : line, 175, 1, COL_GREY);
  drawCentredText("Details on the serial log", 190, 1, COL_GREY);
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);
  runButtons(&back, 1);
}
