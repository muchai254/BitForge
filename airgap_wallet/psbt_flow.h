// Sign PSBT = scan -> parse -> verify against the loaded seed's account ->
// review -> sign (psbt_sign.h) -> show the signed PSBT as an animated QR.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psbt_review.h"

class HDPublicKey;

// Builds the PSBT the signing library gets: only the unsigned transaction,
// each input's verified amount and script (as witness_utxo), and its verified
// derivation. Call only for a Review with ok == true. The scanned PSBT itself
// must never reach uBitcoin: its Tx parser reads input and output counts as a
// single byte, so a transaction with 253+ inputs or outputs is misparsed and
// corrupts the heap. Returns a malloc'ed buffer (caller frees), or nullptr.
uint8_t *psbtBuildClean(const PsbtInfo &info, const Review &r, size_t *outLen);

// Parses and verifies a PSBT against an account key. Fills r (r.ok false
// with r.reason on any refusal, including parse errors). `info` is
// scratch space the caller provides (it's large: keep it on the heap).
void psbtCheck(const uint8_t *psbt, size_t len, const HDPublicKey &account, const uint8_t fp[4], PsbtInfo &info,
               Review &r);

// Main menu -> Sign PSBT (needs a loaded seed).
void signPsbtScreen();
