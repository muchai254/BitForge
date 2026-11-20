#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psbt_review.h"

// Signs every input with the loaded seed. The library only sees the clean PSBT
// (psbtBuildClean), so it signs verified amounts; every signature is checked
// against the input's key before returning. Returns a malloc'ed PSBT (caller
// frees; unsigned tx, witness_utxo and partial signature per input), or nullptr
// with `error` set. Scrubs the stack after the key work.
uint8_t *psbtSign(const PsbtInfo &info, const Review &r, size_t *outLen, char *error, size_t errorCap,
                  void (*progress)(float));

// Shows the signed PSBT as an animated ur:crypto-psbt QR until Done.
void psbtShowSignedScreen(const uint8_t *psbt, size_t len);
