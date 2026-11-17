#include "psbt_parse.h"
#include <string.h>
#include <mbedtls/sha256.h>

const char *psbtParseErrorText(PsbtParseError e) {
  switch (e) {
    case PSBT_OK: return "OK";
    case PSBT_ERR_MAGIC: return "not a PSBT (bad header)";
    case PSBT_ERR_TRUNCATED: return "the PSBT is cut short";
    case PSBT_ERR_FORMAT: return "malformed PSBT field";
    case PSBT_ERR_DUPLICATE: return "a PSBT field appears twice";
    case PSBT_ERR_NO_TX: return "the PSBT has no transaction";
    case PSBT_ERR_TX: return "the transaction inside the PSBT is malformed";
    case PSBT_ERR_TOO_MANY: return "too many inputs or outputs (limit 50 / 20)";
    case PSBT_ERR_VERSION: return "unsupported PSBT version";
    case PSBT_ERR_PREV_TX: return "a previous transaction in the PSBT is malformed";
    case PSBT_ERR_TRAILING: return "extra data after the PSBT";
  }
  return "?";
}

namespace {

// Bounds-checked little-endian reader. After any failure ok is false and
// every further read returns 0, so callers check ok once per step.
struct Reader {
  const uint8_t *b;
  size_t len, pos;
  bool ok;
  Reader(const uint8_t *buf, size_t n) : b(buf), len(n), pos(0), ok(true) {}
  size_t left() const { return ok ? len - pos : 0; }
  bool need(size_t n) {
    if (!ok || len - pos < n) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? b[pos++] : 0; }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = (uint32_t)b[pos] | (uint32_t)b[pos + 1] << 8 | (uint32_t)b[pos + 2] << 16 | (uint32_t)b[pos + 3] << 24;
    pos += 4;
    return v;
  }
  uint64_t u64() {
    uint64_t lo = u32(), hi = u32();
    return lo | hi << 32;
  }
  // CompactSize. Values that can't fit in what's left are refused (they're
  // always lengths or counts here).
  uint64_t varint() {
    uint8_t f = u8();
    uint64_t v;
    if (f < 0xfd) v = f;
    else if (f == 0xfd) { if (!need(2)) return 0; v = (uint64_t)b[pos] | (uint64_t)b[pos + 1] << 8; pos += 2; }
    else if (f == 0xfe) v = u32();
    else v = u64();
    if (ok && v > len) ok = false;
    return ok ? v : 0;
  }
  const uint8_t *bytes(size_t n) {
    if (!need(n)) return nullptr;
    const uint8_t *p = b + pos;
    pos += n;
    return p;
  }
};

// Double SHA-256, byte-reversed into display order.
struct TxidHasher {
  mbedtls_sha256_context c;
  TxidHasher() { mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0); }
  ~TxidHasher() { mbedtls_sha256_free(&c); }
  void add(const uint8_t *p, size_t n) { mbedtls_sha256_update(&c, p, n); }
  void finish(uint8_t txid[32]) {
    uint8_t h[32], h2[32];
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256(h, 32, h2, 0);
    for (int i = 0; i < 32; i++) txid[i] = h2[31 - i];
  }
};

void reversed(const uint8_t *in, uint8_t out[32]) {
  for (int i = 0; i < 32; i++) out[i] = in[31 - i];
}

// The unsigned transaction (global key 0x00): non-witness format, empty
// scriptSigs, 1..50 inputs, 1..20 outputs.
PsbtParseError parseUnsignedTx(const uint8_t *p, size_t n, PsbtInfo &out) {
  Reader r(p, n);
  out.txVersion = r.u32();
  if (r.ok && r.left() > 0 && r.b[r.pos] == 0) return PSBT_ERR_TX; // segwit marker or zero inputs
  uint64_t nIn = r.varint();
  if (!r.ok) return PSBT_ERR_TX;
  if (nIn == 0) return PSBT_ERR_TX;
  if (nIn > PSBT_MAX_INPUTS) return PSBT_ERR_TOO_MANY;
  out.inputCount = (uint32_t)nIn;
  for (uint32_t i = 0; i < nIn; i++) {
    PsbtInput &in = out.inputs[i];
    const uint8_t *txid = r.bytes(32);
    in.vout = r.u32();
    uint64_t scriptLen = r.varint();
    in.sequence = r.u32();
    if (!r.ok) return PSBT_ERR_TX;
    if (scriptLen != 0) return PSBT_ERR_TX; // BIP174: scriptSigs must be empty
    reversed(txid, in.prevTxid);
  }
  uint64_t nOut = r.varint();
  if (!r.ok || nOut == 0) return PSBT_ERR_TX;
  if (nOut > PSBT_MAX_OUTPUTS) return PSBT_ERR_TOO_MANY;
  out.outputCount = (uint32_t)nOut;
  for (uint32_t i = 0; i < nOut; i++) {
    PsbtOutput &o = out.outputs[i];
    o.amount = r.u64();
    o.script.len = (size_t)r.varint();
    o.script.data = r.bytes(o.script.len);
    if (!r.ok) return PSBT_ERR_TX;
  }
  out.locktime = r.u32();
  if (!r.ok || r.left() != 0) return PSBT_ERR_TX;
  return PSBT_OK;
}

// A non_witness_utxo: parses the whole previous transaction (with or without
// witness data), computes its txid, and picks output `vout`.
PsbtParseError parsePrevTx(const uint8_t *p, size_t n, PsbtInput &in) {
  Reader r(p, n);
  TxidHasher h;
  r.u32();
  if (!r.ok) return PSBT_ERR_PREV_TX;
  h.add(p, 4);
  bool segwit = false;
  if (r.left() >= 2 && r.b[r.pos] == 0) {
    if (r.b[r.pos + 1] != 1) return PSBT_ERR_PREV_TX;
    segwit = true;
    r.pos += 2;
  }
  size_t bodyStart = r.pos;
  uint64_t nIn = r.varint();
  if (!r.ok || nIn == 0) return PSBT_ERR_PREV_TX;
  for (uint64_t i = 0; i < nIn && r.ok; i++) {
    r.bytes(36);
    r.bytes((size_t)r.varint());
    r.u32();
  }
  uint64_t nOut = r.varint();
  if (!r.ok || nOut == 0) return PSBT_ERR_PREV_TX;
  in.nonWitnessVoutExists = false;
  for (uint64_t i = 0; i < nOut && r.ok; i++) {
    uint64_t amount = r.u64();
    size_t sl = (size_t)r.varint();
    const uint8_t *s = r.bytes(sl);
    if (r.ok && i == in.vout) {
      in.nonWitnessVoutExists = true;
      in.nonWitnessAmount = amount;
      in.nonWitnessScript = {s, sl};
    }
  }
  if (!r.ok) return PSBT_ERR_PREV_TX;
  h.add(p + bodyStart, r.pos - bodyStart);
  if (segwit) {
    for (uint64_t i = 0; i < nIn && r.ok; i++) {
      uint64_t items = r.varint();
      for (uint64_t k = 0; k < items && r.ok; k++) r.bytes((size_t)r.varint());
    }
  }
  const uint8_t *lt = r.bytes(4);
  if (!r.ok || r.left() != 0) return PSBT_ERR_PREV_TX;
  h.add(lt, 4);
  uint8_t txid[32];
  h.finish(txid);
  in.nonWitnessTxidMatches = memcmp(txid, in.prevTxid, 32) == 0;
  return PSBT_OK;
}

// BIP32 derivation value: 4-byte fingerprint + 4 bytes per path element.
bool parseDerivation(const uint8_t *key, size_t keyLen, const uint8_t *val, size_t valLen, PsbtDerivation &d) {
  if (keyLen != 33 || (key[0] != 0x02 && key[0] != 0x03)) return false;
  if (valLen < 4 || (valLen - 4) % 4 != 0 || (valLen - 4) / 4 > PSBT_MAX_PATH) return false;
  d.pubkey = key;
  memcpy(d.fingerprint, val, 4);
  d.depth = (uint8_t)((valLen - 4) / 4);
  for (int i = 0; i < d.depth; i++) {
    const uint8_t *q = val + 4 + 4 * i;
    d.path[i] = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
  }
  return true;
}

// Adds a derivation; duplicates (same pubkey) are an error. Beyond
// PSBT_MAX_DERIVATIONS only the count goes up (review refuses those).
PsbtParseError addDerivation(const uint8_t *key, size_t keyLen, const uint8_t *val, size_t valLen,
                             PsbtDerivation *list, uint8_t &count) {
  PsbtDerivation d;
  if (!parseDerivation(key, keyLen, val, valLen, d)) return PSBT_ERR_FORMAT;
  int kept = count < PSBT_MAX_DERIVATIONS ? count : PSBT_MAX_DERIVATIONS;
  for (int i = 0; i < kept; i++)
    if (memcmp(list[i].pubkey, d.pubkey, 33) == 0) return PSBT_ERR_DUPLICATE;
  if (count < PSBT_MAX_DERIVATIONS) list[count] = d;
  if (count < 255) count++;
  return PSBT_OK;
}

enum MapKind { MAP_GLOBAL, MAP_INPUT, MAP_OUTPUT };

// Reads one map (until the 0x00 separator). `seen` tracks key types without
// key data, for duplicate detection.
PsbtParseError parseMap(Reader &r, MapKind kind, PsbtInfo &out, uint32_t index, bool &haveTx) {
  uint32_t seen = 0;
  for (;;) {
    uint64_t keyLen = r.varint();
    if (!r.ok) return PSBT_ERR_TRUNCATED;
    if (keyLen == 0) return PSBT_OK;
    const uint8_t *key = r.bytes((size_t)keyLen);
    uint64_t valLen = r.varint();
    const uint8_t *val = r.bytes((size_t)valLen);
    if (!r.ok) return PSBT_ERR_TRUNCATED;
    uint8_t type = key[0];
    const uint8_t *keyData = key + 1;
    size_t keyDataLen = (size_t)keyLen - 1;
    // Single-instance keys (no key data): each type at most once.
    if (keyDataLen == 0 && type < 32) {
      if (seen & (1u << type)) return PSBT_ERR_DUPLICATE;
      seen |= 1u << type;
    }
    if (kind == MAP_GLOBAL) {
      if (type == 0x00) {
        if (keyDataLen) return PSBT_ERR_FORMAT;
        PsbtParseError e = parseUnsignedTx(val, (size_t)valLen, out);
        if (e) return e;
        out.txRaw = val;
        out.txRawLen = (size_t)valLen;
        haveTx = true;
      } else if (type == 0xFB) {
        if (keyDataLen || valLen != 4) return PSBT_ERR_FORMAT;
        if (val[0] || val[1] || val[2] || val[3]) return PSBT_ERR_VERSION;
      } else if (type == 0x01) {
        if (keyDataLen != 78 || valLen < 4 || (valLen - 4) % 4) return PSBT_ERR_FORMAT; // xpub: not used
      } else if (type == 0x02) {
        return PSBT_ERR_VERSION; // PSBT v2 fields
      }
      // Other global keys (proprietary, unknown) are ignored.
    } else if (kind == MAP_INPUT) {
      PsbtInput &in = out.inputs[index];
      switch (type) {
        case 0x00: { // non_witness_utxo
          if (keyDataLen) return PSBT_ERR_FORMAT;
          in.hasNonWitnessUtxo = true;
          PsbtParseError e = parsePrevTx(val, (size_t)valLen, in);
          if (e) return e;
          break;
        }
        case 0x01: { // witness_utxo
          if (keyDataLen) return PSBT_ERR_FORMAT;
          Reader v(val, (size_t)valLen);
          in.witnessAmount = v.u64();
          in.witnessScript.len = (size_t)v.varint();
          in.witnessScript.data = v.bytes(in.witnessScript.len);
          if (!v.ok || v.left()) return PSBT_ERR_FORMAT;
          in.hasWitnessUtxo = true;
          break;
        }
        case 0x02: // partial signature
          if (keyDataLen != 33) return PSBT_ERR_FORMAT;
          in.hasPartialSig = true;
          break;
        case 0x03: // sighash type
          if (keyDataLen || valLen != 4) return PSBT_ERR_FORMAT;
          in.hasSighash = true;
          in.sighash = (uint32_t)val[0] | (uint32_t)val[1] << 8 | (uint32_t)val[2] << 16 | (uint32_t)val[3] << 24;
          break;
        case 0x04: if (keyDataLen) return PSBT_ERR_FORMAT; in.hasRedeemScript = true; break;
        case 0x05: if (keyDataLen) return PSBT_ERR_FORMAT; in.hasWitnessScriptField = true; break;
        case 0x06: {
          PsbtParseError e = addDerivation(keyData, keyDataLen, val, (size_t)valLen, in.derivations, in.derivationCount);
          if (e) return e;
          break;
        }
        case 0x07: case 0x08: in.isFinalized = true; break;
        default: break; // unknown / proprietary: ignored
      }
    } else {
      PsbtOutput &o = out.outputs[index];
      if (type == 0x02) {
        PsbtParseError e = addDerivation(keyData, keyDataLen, val, (size_t)valLen, o.derivations, o.derivationCount);
        if (e) return e;
      }
      // redeem/witness scripts and unknown keys: ignored (change must be P2WPKH).
    }
  }
}

} // namespace

PsbtParseError psbtParse(const uint8_t *buf, size_t len, PsbtInfo &out, size_t *errorPos) {
  memset(&out, 0, sizeof out);
  Reader r(buf, len);
  PsbtParseError e = PSBT_OK;
  bool haveTx = false;
  const uint8_t *magic = r.bytes(5);
  if (!magic || memcmp(magic, "psbt\xff", 5) != 0) e = PSBT_ERR_MAGIC;
  if (!e) e = parseMap(r, MAP_GLOBAL, out, 0, haveTx);
  if (!e && !haveTx) e = PSBT_ERR_NO_TX;
  for (uint32_t i = 0; !e && i < out.inputCount; i++) e = parseMap(r, MAP_INPUT, out, i, haveTx);
  for (uint32_t i = 0; !e && i < out.outputCount; i++) e = parseMap(r, MAP_OUTPUT, out, i, haveTx);
  if (!e && r.left() != 0) e = PSBT_ERR_TRAILING;
  if (errorPos) *errorPos = r.pos;
  return e;
}
