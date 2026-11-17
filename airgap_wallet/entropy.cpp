#include "entropy.h"
#include <Arduino.h>
#include <bootloader_random.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include "secure_mem.h"

// Basic health check of the raw bytes. It can't prove randomness; it catches
// a stuck or badly broken source. Thresholds for 256 random bytes:
//   ones among 2048 bits: mean 1024, sd 22.6; accept 1024 +/- 160 (7 sd)
//   no 32-byte block may equal another
static bool looksRandom(const uint8_t *b, size_t n) {
  size_t ones = 0;
  for (size_t i = 0; i < n; i++) ones += __builtin_popcount(b[i]);
  size_t bits = n * 8;
  if (ones < bits / 2 - 160 || ones > bits / 2 + 160) return false;
  for (size_t a = 0; a + 32 <= n; a += 32) {
    for (size_t c = a + 32; c + 32 <= n; c += 32) {
      if (memcmp(b + a, b + c, 32) == 0) return false;
    }
  }
  return true;
}

bool entropyCollectHardware(uint8_t out[ENTROPY_HW_BYTES]) {
  bootloader_random_enable();
  // Read in small pieces with short pauses so the samples are spread over
  // time rather than taken back to back.
  for (size_t i = 0; i < ENTROPY_HW_BYTES; i += 16) {
    esp_fill_random(out + i, 16);
    delayMicroseconds(200);
  }
  bootloader_random_disable();
  if (!looksRandom(out, ENTROPY_HW_BYTES)) {
    secure_zero(out, ENTROPY_HW_BYTES);
    return false;
  }
  return true;
}

void entropyMix(const uint8_t *hw, size_t hwLen, const char *dice, size_t diceLen, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, hw, hwLen);
  if (diceLen) mbedtls_sha256_update(&ctx, (const unsigned char *)dice, diceLen);
  mbedtls_sha256_update(&ctx, (const unsigned char *)ENTROPY_DOMAIN, strlen(ENTROPY_DOMAIN));
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx); // zeroises the context
}
