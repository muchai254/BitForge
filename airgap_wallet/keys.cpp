#include "keys.h"
#include <Bitcoin.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include "descriptor.h"
#include "display.h"
#include "secure_mem.h"
#include "seedqr.h"
#include "ui.h"

// --- Public test vector. NOT a secret: everyone knows it. ---
static const char TEST_MNEMONIC[] =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

// Expected values, from Sparrow 2.5.5 on signet (an independent tool).
static const char EXPECT_FINGERPRINT[] = "73c5da0a";
static const char EXPECT_TPUB[] =
    "tpubDC8msFGeGuwnKG9Upg7DM2b4DaRqg3CUZa5g8v2SRQ6K4NSkxUgd7HsL2XVWbVm39yBA4LAxysQAm397zwQSQoQgewGiYZqrA9DsP4zbQ1M";
static const char EXPECT_RECEIVE0[] = "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl";
static const char EXPECT_CHANGE0[] = "tb1q9u62588spffmq4dzjxsr5l297znf3z6j5p2688";
// Watch-only export of that account. The checksum was computed on the PC
// with a port of Bitcoin Core's algorithm checked against BIP380's vectors.
static const char EXPECT_DESCRIPTOR[] =
    "wpkh([73c5da0a/84h/1h/0h]tpubDC8msFGeGuwnKG9Upg7DM2b4DaRqg3CUZa5g8v2SRQ6K4NSkxUgd7HsL2XVWbVm39yBA4LAxysQAm397zwQS"
    "QoQgewGiYZqrA9DsP4zbQ1M/<0;1>/*)#lxek0ef2";

// BIP84 account on test networks: m/84'/1'/0'.
static const char ACCOUNT_PATH[] = "m/84h/1h/0h";

struct TestVectorResult {
  uint8_t fp[4];
  char fingerprint[9];
  char tpub[120];
  char receive0[80];
  char change0[80];
  uint32_t seedMs, deriveMs;
};

static bool check(const char *label, const char *got, const char *expected) {
  bool ok = strcmp(got, expected) == 0;
  Serial.printf("  %-18s %s  %s\n", label, got, ok ? "OK" : "FAIL");
  if (!ok) Serial.printf("  %-18s %s  (expected)\n", "", expected);
  return ok;
}

static int progressPct = -1;
static void pbkdf2Progress(float f) {
  int pct = (int)(f * 100);
  if (pct / 10 == progressPct / 10) return;
  progressPct = pct;
  tft.fillRect(20, 150, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

// All uBitcoin key work happens in here, in its own stack frames, so that
// afterwards the caller can check (and scrub) what it left on the stack.
__attribute__((noinline)) static void deriveTestVector(TestVectorResult &r) {
  progressPct = -1;
  uint32_t t0 = millis();
  HDPrivateKey root(TEST_MNEMONIC, strlen(TEST_MNEMONIC), "", 0, &Signet, pbkdf2Progress);
  r.seedMs = millis() - t0;

  root.fingerprint(r.fp);
  snprintf(r.fingerprint, sizeof r.fingerprint, "%02x%02x%02x%02x", r.fp[0], r.fp[1], r.fp[2], r.fp[3]);

  t0 = millis();
  HDPrivateKey account = root.derive(ACCOUNT_PATH);
  HDPublicKey accountPub = account.xpub(); // type P2WPKH (set by derive at 84')
  // uBitcoin serialises a P2WPKH account as vpub; Sparrow shows the plain
  // BIP32 version bytes (tpub). Same key, so compare with the type cleared.
  HDPublicKey plain = accountPub;
  plain.type = UNKNOWN_TYPE;
  plain.xpub(r.tpub, sizeof r.tpub);
  accountPub.child(0).child(0).address(r.receive0, sizeof r.receive0);
  accountPub.child(1).child(0).address(r.change0, sizeof r.change0);
  r.deriveMs = millis() - t0;
  // root/account destructors zero num and chainCode (uBitcoin memzero).
}

__attribute__((noinline)) void keysFingerprint(const char *mnemonic, size_t mnemonicLen,
                                               const char *passphrase, size_t passLen, uint8_t fp[4],
                                               void (*progress)(float)) {
  HDPrivateKey root(mnemonic, mnemonicLen, passphrase, passLen, &Signet, progress);
  root.fingerprint(fp);
  // root's destructor zeroes its key; patch 7 (docs/patches.md) wiped the seed in fromMnemonic.
}

__attribute__((noinline)) void keysAccount(const char *mnemonic, size_t mnemonicLen, const char *passphrase,
                                           size_t passLen, AccountInfo &out, void (*progress)(float)) {
  HDPrivateKey root(mnemonic, mnemonicLen, passphrase, passLen, &Signet, progress);
  root.fingerprint(out.fingerprint);
  HDPrivateKey account = root.derive(ACCOUNT_PATH);
  HDPublicKey accountPub = account.xpub(); // P2WPKH type: addresses are tb1q...
  HDPublicKey plain = accountPub;
  plain.type = UNKNOWN_TYPE; // serialise as tpub, not vpub (see keysRunTestVector)
  plain.xpub(out.tpub, sizeof out.tpub);
  HDPublicKey receiveChain = accountPub.child(0);
  for (int i = 0; i < KEYS_ADDRESSES; i++) receiveChain.child(i).address(out.receive[i], sizeof out.receive[i]);
  HDPublicKey changeChain = accountPub.child(1);
  for (int i = 0; i < 2; i++) changeChain.child(i).address(out.change[i], sizeof out.change[i]);
}

// --- Leak check: are raw copies of the test seed / master key left on the stack? ---
// Secret material of the test vector, computed independently with mbedtls:
// BIP39 seed (64 B), master private key (32 B), master chain code (32 B).
#define LEAK_SECRET_LEN 128
#define LEAK_CHUNK 16

static bool computeTestSecrets(uint8_t out[LEAK_SECRET_LEN]) {
  static const char salt[] = "mnemonic"; // BIP39: "mnemonic" + empty passphrase
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA512, (const unsigned char *)TEST_MNEMONIC,
                                    strlen(TEST_MNEMONIC), (const unsigned char *)salt, strlen(salt),
                                    2048, 64, out) != 0) {
    return false;
  }
  static const char key[] = "Bitcoin seed"; // BIP32 master key derivation
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), (const unsigned char *)key,
                         strlen(key), out, 64, out + 64) == 0;
}

// Counts 16-byte chunks of `secret` found anywhere in this task's free stack
// (from the bottom up to just below the caller's frame). Reads only.
__attribute__((noinline)) static int countStackLeaks(const uint8_t *secret) {
  uint8_t here;
  const uint8_t *bottom = pxTaskGetStackStart(NULL);
  const uint8_t *top = (const uint8_t *)((uintptr_t)&here - 64); // stay below this function's own frame
  int found = 0;
  for (int c = 0; c < LEAK_SECRET_LEN / LEAK_CHUNK; c++) {
    const uint8_t *needle = secret + c * LEAK_CHUNK;
    for (const uint8_t *p = bottom; p + LEAK_CHUNK <= top; p++) {
      if (p[0] == needle[0] && memcmp(p, needle, LEAK_CHUNK) == 0) {
        found++;
        break; // count each chunk once
      }
    }
  }
  return found;
}

// SeedQR of the test mnemonic, from the spec: "abandon" is word 0 and "about"
// word 3, so Standard = "0000" x 11 + "0003"; Compact = the 16 entropy bytes,
// all zero. Both must also decode back to the mnemonic.
static bool seedQrTestVector() {
  char expectStd[49];
  for (int i = 0; i < 11; i++) memcpy(expectStd + 4 * i, "0000", 4);
  memcpy(expectStd + 44, "0003", 5);
  static const uint8_t zeros[16] = {0};

  SecretBuffer<97> q;
  SecretBuffer<SESSION_MNEMONIC_CAP> back;
  SeedQrFormat fmt;
  size_t n = seedqrEncode(TEST_MNEMONIC, SEEDQR_STANDARD, q);
  bool stdOk = n == 48 && memcmp(q.data(), expectStd, 48) == 0;
  bool stdBack = seedqrDecode(q.data(), n, back, &fmt) == 12 && fmt == SEEDQR_STANDARD &&
                 strcmp(back.c_str(), TEST_MNEMONIC) == 0;
  n = seedqrEncode(TEST_MNEMONIC, SEEDQR_COMPACT, q);
  bool cmpOk = n == 16 && memcmp(q.data(), zeros, 16) == 0;
  bool cmpBack = seedqrDecode(q.data(), n, back, &fmt) == 12 && fmt == SEEDQR_COMPACT &&
                 strcmp(back.c_str(), TEST_MNEMONIC) == 0;
  // A bad checksum word must be refused: 12 x "0000" is "abandon" x 12.
  char bad[49];
  for (int i = 0; i < 12; i++) memcpy(bad + 4 * i, "0000", 4);
  bool badRefused = seedqrDecode((const uint8_t *)bad, 48, back, &fmt) == 0 && fmt == SEEDQR_NONE;
  Serial.printf("  %-18s standard %s, decodes back %s; compact %s, decodes back %s; bad checksum %s\n", "SeedQR",
                stdOk ? "OK" : "FAIL", stdBack ? "OK" : "FAIL", cmpOk ? "OK" : "FAIL", cmpBack ? "OK" : "FAIL",
                badRefused ? "refused" : "ACCEPTED");
  return stdOk && stdBack && cmpOk && cmpBack && badRefused;
}

// Detector self-test: deliberately leaves a copy of the secrets in its stack
// frame and returns, like careless library code would. countStackLeaks()
// must then find all of it.
__attribute__((noinline)) static void plantStackLeak(const uint8_t *secret) {
  volatile uint8_t copy[LEAK_SECRET_LEN];
  for (int i = 0; i < LEAK_SECRET_LEN; i++) copy[i] = secret[i];
  (void)copy[0];
}

// The checked code runs one padding block deeper than the checker, so that
// countStackLeaks()' own frame (which lands right below the caller) only
// overwrites padding, never the evidence.
#define LEAK_PAD 512
__attribute__((noinline)) static void deriveTestVectorPadded(TestVectorResult &r) {
  volatile uint8_t pad[LEAK_PAD];
  pad[0] = 0;
  deriveTestVector(r);
  (void)pad[0];
}
__attribute__((noinline)) static void plantStackLeakPadded(const uint8_t *secret) {
  volatile uint8_t pad[LEAK_PAD];
  pad[0] = 0;
  plantStackLeak(secret);
  (void)pad[0];
}

bool keysRunTestVector() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Key test vector", 8, 2, COL_BTC_ORANGE);
  drawTextWrapped("Public BIP39 test mnemonic, signet. Not a real wallet.", 10, 40, tft.width() - 20, 1, COL_GREY);
  drawCentredText("Deriving seed (PBKDF2)...", 130, 1, COL_WHITE);
  tft.drawRect(19, 149, tft.width() - 38, 14, COL_GREY);

  Serial.println("[keys] test vector: public BIP39 mnemonic 'abandon x11 about', signet, m/84'/1'/0'");
  if (!checkMnemonic(TEST_MNEMONIC)) {
    Serial.println("  mnemonic checksum: FAIL");
    return false;
  }

  // Reference secrets for the leak check (heap, wiped below). Computing them
  // uses the stack too, so scrub before the real derivation runs.
  uint8_t *secrets = (uint8_t *)malloc(LEAK_SECRET_LEN);
  bool haveSecrets = secrets && computeTestSecrets(secrets);
  secure_scrub_stack();

  TestVectorResult *r = (TestVectorResult *)calloc(1, sizeof(TestVectorResult));
  if (!r) {
    if (secrets) { secure_zero(secrets, LEAK_SECRET_LEN); free(secrets); }
    return false;
  }
  deriveTestVectorPadded(*r);

  int leaksAfterLib = haveSecrets ? countStackLeaks(secrets) : -1;
  size_t scrubbed = secure_scrub_stack();
  int leaksAfterScrub = haveSecrets ? countStackLeaks(secrets) : -1;
  int planted = -1, plantedAfterScrub = -1;
  if (haveSecrets) {
    plantStackLeakPadded(secrets);
    planted = countStackLeaks(secrets);
    secure_scrub_stack();
    plantedAfterScrub = countStackLeaks(secrets);
  }
  if (secrets) {
    secure_zero(secrets, LEAK_SECRET_LEN);
    free(secrets);
  }

  bool ok = true;
  ok &= check("master fingerprint", r->fingerprint, EXPECT_FINGERPRINT);
  ok &= check("account tpub", r->tpub, EXPECT_TPUB);
  ok &= check("receive 0/0", r->receive0, EXPECT_RECEIVE0);
  ok &= check("change 1/0", r->change0, EXPECT_CHANGE0);
  char desc[220];
  if (!descriptorWpkhAccount(r->fp, r->tpub, desc, sizeof desc)) desc[0] = 0;
  ok &= check("descriptor", desc, EXPECT_DESCRIPTOR);
  ok &= seedQrTestVector();
  Serial.printf("[keys] test vector %s (PBKDF2 %lu ms, derivation %lu ms)\n", ok ? "PASS" : "FAIL",
                (unsigned long)r->seedMs, (unsigned long)r->deriveMs);
  Serial.printf("[keys] stack leak check (16-byte chunks of seed/master key/chain code, 8 total): "
                "%d after derivation (library patch only), %d after scrub (%u bytes scrubbed)\n",
                leaksAfterLib, leaksAfterScrub, (unsigned)scrubbed);
  Serial.printf("[keys] leak detector self-test: planted copy found %d of 8, after scrub %d\n", planted,
                plantedAfterScrub);
  bool detectorOk = planted == LEAK_SECRET_LEN / LEAK_CHUNK && plantedAfterScrub == 0;

  tft.fillRect(0, 125, tft.width(), 45, COL_BLACK);
  drawCentredText(ok ? "PASS" : "FAIL", 130, 3, ok ? COL_GREEN : COL_RED);
  int y = 175;
  y = drawTextWrapped("fingerprint", 10, y, 220, 1, COL_GREY);
  y = drawTextWrapped(r->fingerprint, 10, y, 220, 1, COL_WHITE) + 4;
  y = drawTextWrapped("receive 0/0", 10, y, 220, 1, COL_GREY);
  y = drawTextWrapped(r->receive0, 10, y, 220, 1, COL_WHITE) + 4;
  y = drawTextWrapped("change 1/0", 10, y, 220, 1, COL_GREY);
  y = drawTextWrapped(r->change0, 10, y, 220, 1, COL_WHITE) + 4;
  char leakLine[80];
  snprintf(leakLine, sizeof leakLine, "stack leaks: %d, after scrub %d (detector %s)", leaksAfterLib,
           leaksAfterScrub, detectorOk ? "ok" : "BROKEN");
  drawTextWrapped(leakLine, 10, y, 220, 1, leaksAfterScrub == 0 && detectorOk ? COL_GREY : COL_RED);
  free(r);
  return ok && leaksAfterScrub == 0 && detectorOk;
}
