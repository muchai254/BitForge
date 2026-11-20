// Sign PSBT = scan -> parse -> verify against the loaded seed's account ->
// review -> sign (psbt_sign.h) -> show the signed PSBT as an animated QR.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psbt_review.h"

class HDPublicKey;

// Builds the PSBT the signing library gets: the unsigned transaction plus each
// input's verified amount and script (as witness_utxo) and derivation. Only for
// a Review with ok == true. The scanned PSBT itself must never reach uBitcoin:
// its Tx parser reads input/output counts as one byte, so 253+ inputs or
// outputs are misparsed and corrupt the heap. Returns a malloc'ed buffer
// (caller frees), or nullptr.
uint8_t *psbtBuildClean(const PsbtInfo &info, const Review &r, size_t *outLen);

// Parses and verifies a PSBT; r.ok is false with r.reason on any refusal
// (parse errors included). `info` is caller scratch space (large: keep it on
// the heap).
void psbtCheck(const uint8_t *psbt, size_t len, const HDPublicKey &account, const uint8_t fp[4], PsbtInfo &info,
               Review &r);

// Main menu -> Sign PSBT (needs a loaded seed).
void signPsbtScreen();
