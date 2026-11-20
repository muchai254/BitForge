// Entropy for new seeds.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ENTROPY_HW_BYTES 256 // collected from the hardware RNG
#define ENTROPY_MIN_DICE 99  // d6 rolls; 99 * log2(6) = 255.9 bits
#define ENTROPY_DOMAIN "BitForge-v1"

// Fills out[0..ENTROPY_HW_BYTES) from the ESP32 hardware RNG with its true
// random source enabled (bootloader_random_enable(): SAR ADC noise; without
// it, and with the radio off, esp_random() is only a pseudo-random generator).
// The touch panel can't be read while the source is on; it's switched off
// again before returning. Returns false if the bytes fail a basic sanity
// check (stuck or repeating output); the caller must then not make a seed.
bool entropyCollectHardware(uint8_t out[ENTROPY_HW_BYTES]);

// out = SHA-256(hw || dice || ENTROPY_DOMAIN). dice may be empty.
void entropyMix(const uint8_t *hw, size_t hwLen, const char *dice, size_t diceLen, uint8_t out[32]);
