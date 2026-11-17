// Signs an approved PSBT and shows the result as an animated QR.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psbt_review.h"

// Signs every input of a verified PSBT with the loaded seed. The library only
// ever sees the clean PSBT (psbtBuildClean), so it signs verified amounts.
// Every signature is checked against the input's key before anything leaves
// this function. On success returns a malloc'ed output PSBT (caller frees; unsigned tx,
// witness_utxo and partial signature per input); on failure nullptr and
// `error` says why. Scrubs the stack after the key work.
uint8_t *psbtSign(const PsbtInfo &info, const Review &r, size_t *outLen, char *error, size_t errorCap,
                  void (*progress)(float));

// Shows the signed PSBT as an animated ur:crypto-psbt QR until Done.
void psbtShowSignedScreen(const uint8_t *psbt, size_t len);
