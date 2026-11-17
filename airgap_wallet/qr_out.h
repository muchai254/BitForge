// Drawing QR codes on the TFT.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define QR_MAX_VERSION 20 // 97 modules; anything bigger can't be scanned from a 240 px screen
#define QR_QUIET 4        // quiet zone, in modules (QR spec minimum)

// ECC levels (same values as the encoder's ECC_* constants).
#define QR_ECC_LOW 0
#define QR_ECC_MEDIUM 1

struct QrInfo {
  int version;  // 0 if the data didn't fit
  int size;     // modules per side
  int modulePx; // pixels per module
};

// Encodes data with the smallest version (1..QR_MAX_VERSION) that fits at the
// given ECC level and draws it centred in the box (x, y, w, h): white
// background, QR_QUIET modules of quiet zone, the largest whole-pixel module
// size that fits. The encoder's buffer is wiped afterwards (SeedQRs are
// secret; callers should also scrub the stack). Returns version 0 if it
// doesn't fit (nothing is drawn).
QrInfo qrDraw(const uint8_t *data, size_t len, int x, int y, int w, int h, uint8_t ecc);

// Checks the encoder's capacity limits at known boundaries (ISO 18004 byte
// mode: v1-L 17, v1-M 14, v2-L 32, v7-L 154, v10-L 271 bytes): the capacity
// must encode, one more byte must be refused. Prints the result to Serial.
bool qrSelfTest();
