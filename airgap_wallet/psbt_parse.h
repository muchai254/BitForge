// Strict BIP174 (PSBT version 0) parser, independent of uBitcoin's. It reads
// every key-value pair with bounds checks, recomputes the txid of every
// non_witness_utxo, and records what each input and output claims. It decides
// nothing about ownership or amounts: psbt_review.cpp does that.
//
// The parsed fields point into the caller's PSBT buffer, which must outlive
// the PsbtInfo.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PSBT_MAX_INPUTS 50   // prototype limits
#define PSBT_MAX_OUTPUTS 20
#define PSBT_MAX_DERIVATIONS 2 // per input/output (P2WPKH has one key)
#define PSBT_MAX_PATH 10

struct PsbtDerivation {
  const uint8_t *pubkey; // 33 bytes (compressed SEC)
  uint8_t fingerprint[4];
  uint32_t path[PSBT_MAX_PATH];
  uint8_t depth;
};

struct PsbtScript {
  const uint8_t *data;
  size_t len;
};

struct PsbtInput {
  uint8_t prevTxid[32]; // as displayed (big-endian hex order)
  uint32_t vout, sequence;
  // non_witness_utxo (0x00): the whole previous transaction.
  bool hasNonWitnessUtxo;
  bool nonWitnessTxidMatches; // its txid == prevTxid
  bool nonWitnessVoutExists;  // it has an output number vout
  uint64_t nonWitnessAmount;
  PsbtScript nonWitnessScript;
  // witness_utxo (0x01): just the claimed output.
  bool hasWitnessUtxo;
  uint64_t witnessAmount;
  PsbtScript witnessScript;
  bool hasSighash;
  uint32_t sighash;
  uint8_t derivationCount; // all 0x06 entries (only the first PSBT_MAX_DERIVATIONS are kept)
  PsbtDerivation derivations[PSBT_MAX_DERIVATIONS];
  bool hasPartialSig, hasRedeemScript, hasWitnessScriptField, isFinalized;
};

struct PsbtOutput {
  uint64_t amount;
  PsbtScript script;
  uint8_t derivationCount;
  PsbtDerivation derivations[PSBT_MAX_DERIVATIONS];
};

struct PsbtInfo {
  const uint8_t *txRaw; // the unsigned transaction's bytes
  size_t txRawLen;
  uint32_t txVersion, locktime;
  uint32_t inputCount, outputCount;
  PsbtInput inputs[PSBT_MAX_INPUTS];
  PsbtOutput outputs[PSBT_MAX_OUTPUTS];
};

enum PsbtParseError {
  PSBT_OK = 0,
  PSBT_ERR_MAGIC,       // doesn't start with "psbt\xff"
  PSBT_ERR_TRUNCATED,   // a length runs past the end
  PSBT_ERR_FORMAT,      // malformed field (wrong key or value size, bad varint)
  PSBT_ERR_DUPLICATE,   // the same key twice in a map
  PSBT_ERR_NO_TX,
  PSBT_ERR_TX,          // the unsigned transaction is malformed or not unsigned
  PSBT_ERR_TOO_MANY,
  PSBT_ERR_VERSION,     // PSBT version other than 0
  PSBT_ERR_PREV_TX,     // a non_witness_utxo is malformed
  PSBT_ERR_TRAILING,    // bytes after the last output map
};
const char *psbtParseErrorText(PsbtParseError e);

// Parses buf[0..len). On error, *errorPos (if given) is the byte offset.
PsbtParseError psbtParse(const uint8_t *buf, size_t len, PsbtInfo &out, size_t *errorPos = nullptr);
