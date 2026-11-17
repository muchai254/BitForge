// Independent verification of a parsed PSBT. Everything shown on the review
// screens is computed here, on the device, from the PSBT bytes and our own
// account key. Nothing Sparrow claims is trusted without a check.
//
// Rules:
// - Every input is ours: a BIP32 derivation with our fingerprint, path
//   m/84'/1'/0'/{0,1}/i (i <= 10000), the re-derived pubkey equals the one
//   in the PSBT, and the coin's script is our P2WPKH script. Otherwise the
//   whole transaction is refused.
// - Input amounts: more than one input -> each needs a non_witness_utxo whose
//   txid matches the prevout. A witness_utxo amount alone can be a lie, and
//   with several inputs that can hide a huge fee (CVE-2020-14199). Exactly
//   one input -> witness_utxo is accepted if there's no non_witness_utxo
//   (amountsVerified = false: the review shows a note). If both are present
//   they must agree.
// - Sighash: absent or SIGHASH_ALL only. No finalized inputs, partial
//   signatures, redeem or witness scripts.
// - Outputs: P2WPKH, P2WSH, P2PKH or P2SH only (addresses this firmware can
//   encode and show correctly). Change only if fingerprint + path
//   m/84'/1'/0'/1/i + re-derived script match byte for byte; anything else
//   is a recipient (a verified m/84'/1'/0'/0/i output is a recipient shown
//   as "to yourself").
// - Fee = inputs - outputs >= 0, with overflow checks; warnings for high fees.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psbt_parse.h"

class HDPublicKey;

#define REVIEW_MAX_INDEX 10000
#define REVIEW_ADDR_LEN 96

enum OutputKind : uint8_t {
  OUT_RECIPIENT,   // someone else (or unverifiable)
  OUT_SELF,        // verified: our receive chain m/84'/1'/0'/0/i
  OUT_CHANGE,      // verified: our change chain m/84'/1'/0'/1/i
};

struct ReviewOutput {
  OutputKind kind;
  uint32_t index;            // address index for OUT_SELF / OUT_CHANGE
  bool claimFailed;          // the PSBT says it's ours, but it isn't
  uint64_t amount;
  char address[REVIEW_ADDR_LEN];
};

struct Review {
  bool ok;                 // false: refuse; reason says why
  char reason[120];
  uint32_t inputCount, outputCount;
  uint64_t inputAmounts[PSBT_MAX_INPUTS]; // verified as far as the rules allow (see amountsVerified)
  uint64_t inputTotal, outputTotal, fee;
  uint64_t sentTotal;      // all non-change outputs (recipients + to self)
  uint64_t changeTotal;
  uint32_t vsize;          // estimated after signing (P2WPKH witnesses)
  uint32_t feeRateX10;     // sat/vB * 10
  bool amountsVerified;    // every input amount came from a verified previous tx
  bool highFee;            // fee > 1% of sent, > 50,000 sat, or > 500 sat/vB
  char highFeeWhy[64];
  bool rbf;
  uint32_t locktime;
  ReviewOutput outputs[PSBT_MAX_OUTPUTS];
};

// account: m/84'/1'/0' of the loaded seed; fp: its master fingerprint.
void psbtReview(const PsbtInfo &psbt, const HDPublicKey &account, const uint8_t fp[4], Review &out);

// "0.00050000" (8 decimals) and "50,000".
void formatBtc(uint64_t sats, char *out, size_t cap);
void formatSats(uint64_t sats, char *out, size_t cap);
