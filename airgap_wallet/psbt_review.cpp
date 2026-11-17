#include "psbt_review.h"
#include <Bitcoin.h>
#include <Conversion.h>
#include <Hash.h>
#include <string.h>
#include <utility/segwit_addr.h>

static const uint64_t MAX_MONEY = 2100000000000000ull; // 21M BTC in sats
static const uint32_t HARDENED = 0x80000000u;
static const uint32_t PATH_PREFIX[3] = {84 | HARDENED, 1 | HARDENED, 0 | HARDENED}; // m/84'/1'/0'

void formatSats(uint64_t sats, char *out, size_t cap) {
  char digits[24];
  int n = 0;
  do {
    digits[n++] = (char)('0' + sats % 10);
    sats /= 10;
  } while (sats);
  size_t o = 0;
  for (int i = n - 1; i >= 0 && o + 1 < cap; i--) {
    out[o++] = digits[i];
    if (i && i % 3 == 0 && o + 1 < cap) out[o++] = ',';
  }
  out[o] = 0;
}

void formatBtc(uint64_t sats, char *out, size_t cap) {
  uint64_t whole = sats / 100000000ull;
  uint32_t frac = (uint32_t)(sats % 100000000ull);
  char w[24];
  int n = 0;
  do {
    w[n++] = (char)('0' + whole % 10);
    whole /= 10;
  } while (whole);
  size_t o = 0;
  while (n && o + 1 < cap) out[o++] = w[--n];
  if (o + 1 < cap) out[o++] = '.';
  for (uint32_t d = 10000000; d && o + 1 < cap; d /= 10) out[o++] = (char)('0' + frac / d % 10);
  out[o] = 0;
}

static void fail(Review &r, const char *fmt, unsigned a = 0, unsigned b = 0) {
  r.ok = false;
  snprintf(r.reason, sizeof r.reason, fmt, a, b);
}

// Our P2WPKH script for a compressed pubkey: 0x00 0x14 <hash160>.
static void p2wpkhScript(const uint8_t pub[33], uint8_t script[22]) {
  script[0] = 0x00;
  script[1] = 0x14;
  hash160(pub, 33, script + 2);
}

enum Ownership { NOT_CLAIMED, VERIFIED, CLAIM_FAILED };

// Looks for a derivation with our fingerprint and checks it completely:
// path m/84'/1'/0'/<chain>/<index> with chain 0 or 1 and index <= 10000,
// and the pubkey we derive for that path equals the PSBT's pubkey.
static Ownership checkDerivation(const PsbtDerivation *list, uint8_t count, const uint8_t fp[4],
                                 const HDPublicKey chains[2], uint32_t &chain, uint32_t &index, uint8_t script[22]) {
  int kept = count < PSBT_MAX_DERIVATIONS ? count : PSBT_MAX_DERIVATIONS;
  for (int i = 0; i < kept; i++) {
    const PsbtDerivation &d = list[i];
    if (memcmp(d.fingerprint, fp, 4) != 0) continue;
    if (d.depth != 5 || d.path[0] != PATH_PREFIX[0] || d.path[1] != PATH_PREFIX[1] || d.path[2] != PATH_PREFIX[2])
      return CLAIM_FAILED;
    if (d.path[3] > 1 || d.path[4] > REVIEW_MAX_INDEX) return CLAIM_FAILED;
    HDPublicKey child = chains[d.path[3]].child(d.path[4]);
    uint8_t sec[33];
    if (child.sec(sec, sizeof sec) != 33 || memcmp(sec, d.pubkey, 33) != 0) return CLAIM_FAILED;
    chain = d.path[3];
    index = d.path[4];
    p2wpkhScript(sec, script);
    return VERIFIED;
  }
  return NOT_CLAIMED;
}

static bool scriptIs(const PsbtScript &s, const uint8_t *want, size_t n) {
  return s.len == n && memcmp(s.data, want, n) == 0;
}

// Encodes the output script as a signet address. Only types this firmware
// can encode correctly are accepted; for segwit, the encoding is decoded
// again and compared (guards against an encoder bug showing a wrong address).
static bool scriptToAddress(const PsbtScript &s, char *out, size_t cap) {
  const uint8_t *p = s.data;
  if ((s.len == 22 && p[0] == 0x00 && p[1] == 0x14) || (s.len == 34 && p[0] == 0x00 && p[1] == 0x20)) {
    char addr[REVIEW_ADDR_LEN];
    if (!segwit_addr_encode(addr, Signet.bech32, 0, p + 2, s.len - 2)) return false;
    int ver;
    uint8_t prog[40];
    size_t progLen;
    if (!segwit_addr_decode(&ver, prog, &progLen, Signet.bech32, addr) || ver != 0 || progLen != s.len - 2 ||
        memcmp(prog, p + 2, progLen) != 0)
      return false;
    if (strlen(addr) + 1 > cap) return false;
    strcpy(out, addr);
    return true;
  }
  uint8_t payload[21];
  if (s.len == 25 && p[0] == 0x76 && p[1] == 0xa9 && p[2] == 0x14 && p[23] == 0x88 && p[24] == 0xac) {
    payload[0] = Signet.p2pkh;
    memcpy(payload + 1, p + 3, 20);
  } else if (s.len == 23 && p[0] == 0xa9 && p[1] == 0x14 && p[22] == 0x87) {
    payload[0] = Signet.p2sh;
    memcpy(payload + 1, p + 2, 20);
  } else {
    return false; // taproot, OP_RETURN, bare scripts...: not supported in the prototype
  }
  memset(out, 0, cap);
  return toBase58Check(payload, sizeof payload, out, cap) > 0 && out[0];
}

static size_t varintSize(uint64_t n) { return n < 0xfd ? 1 : n <= 0xffff ? 3 : 5; }

void psbtReview(const PsbtInfo &psbt, const HDPublicKey &account, const uint8_t fp[4], Review &r) {
  memset(&r, 0, sizeof r);
  r.ok = true;
  r.inputCount = psbt.inputCount;
  r.outputCount = psbt.outputCount;
  r.locktime = psbt.locktime;
  r.amountsVerified = true;
  if (psbt.txVersion != 1 && psbt.txVersion != 2) return fail(r, "Unsupported transaction version %u.", psbt.txVersion);

  const HDPublicKey chains[2] = {account.child(0), account.child(1)};

  for (uint32_t i = 0; i < psbt.inputCount; i++) {
    const PsbtInput &in = psbt.inputs[i];
    unsigned n = i + 1;
    if (in.sequence < 0xfffffffeu) r.rbf = true;
    if (in.isFinalized || in.hasPartialSig) return fail(r, "Input %u is already signed.", n);
    if (in.hasRedeemScript || in.hasWitnessScriptField)
      return fail(r, "Input %u isn't a plain P2WPKH input (it has a script). Not supported.", n);
    if (in.hasSighash && in.sighash != 1)
      return fail(r, "Input %u asks for sighash type 0x%x. Only SIGHASH_ALL is allowed.", n, in.sighash);
    if (in.derivationCount != 1) return fail(r, "Input %u has %u keys. Only single-key inputs are supported.", n, in.derivationCount);
    uint32_t chain, index;
    uint8_t ours[22];
    Ownership own = checkDerivation(in.derivations, in.derivationCount, fp, chains, chain, index, ours);
    if (own == NOT_CLAIMED) return fail(r, "Input %u isn't from this wallet (other fingerprint).", n);
    if (own == CLAIM_FAILED) return fail(r, "Input %u claims to be ours, but its key doesn't match this wallet.", n);

    // Amount: see the input-amount rule in psbt_review.h.
    uint64_t amount;
    if (in.hasNonWitnessUtxo) {
      if (!in.nonWitnessTxidMatches) return fail(r, "Input %u: the previous transaction doesn't match the coin it spends.", n);
      if (!in.nonWitnessVoutExists) return fail(r, "Input %u: the previous transaction has no output %u.", n, in.vout);
      if (!scriptIs(in.nonWitnessScript, ours, sizeof ours)) return fail(r, "Input %u: the coin isn't paid to this wallet.", n);
      amount = in.nonWitnessAmount;
      if (in.hasWitnessUtxo && (in.witnessAmount != amount || !scriptIs(in.witnessScript, ours, sizeof ours)))
        return fail(r, "Input %u: the two descriptions of the coin disagree (amount or script).", n);
    } else if (psbt.inputCount > 1) {
      return fail(r, "Input %u has no previous transaction. With more than one input it's required (fee-lie protection).", n);
    } else if (in.hasWitnessUtxo) {
      if (!scriptIs(in.witnessScript, ours, sizeof ours)) return fail(r, "Input %u: the coin isn't paid to this wallet.", n);
      amount = in.witnessAmount;
      r.amountsVerified = false;
    } else {
      return fail(r, "Input %u has no amount information.", n);
    }
    if (amount > MAX_MONEY || r.inputTotal + amount > MAX_MONEY) return fail(r, "Input %u: impossible amount.", n);
    r.inputAmounts[i] = amount;
    r.inputTotal += amount;
  }

  uint64_t outputBytes = 0;
  for (uint32_t i = 0; i < psbt.outputCount; i++) {
    const PsbtOutput &o = psbt.outputs[i];
    ReviewOutput &ro = r.outputs[i];
    unsigned n = i + 1;
    if (o.amount > MAX_MONEY || r.outputTotal + o.amount > MAX_MONEY) return fail(r, "Output %u: impossible amount.", n);
    r.outputTotal += o.amount;
    ro.amount = o.amount;
    outputBytes += 8 + varintSize(o.script.len) + o.script.len;
    if (!scriptToAddress(o.script, ro.address, sizeof ro.address))
      return fail(r, "Output %u has an address type this prototype can't show (only tb1q, m/n, 2).", n);
    ro.kind = OUT_RECIPIENT;
    if (o.derivationCount > PSBT_MAX_DERIVATIONS) {
      ro.claimFailed = true;
    } else {
      uint32_t chain, index;
      uint8_t ours[22];
      Ownership own = checkDerivation(o.derivations, o.derivationCount, fp, chains, chain, index, ours);
      if (own == VERIFIED && scriptIs(o.script, ours, sizeof ours)) {
        ro.kind = chain == 1 ? OUT_CHANGE : OUT_SELF;
        ro.index = index;
      } else if (own != NOT_CLAIMED) {
        ro.claimFailed = true; // says it's ours, isn't: shown as a recipient, with a warning
      }
    }
    if (ro.kind == OUT_CHANGE) r.changeTotal += o.amount;
    else r.sentTotal += o.amount;
  }

  if (r.outputTotal > r.inputTotal) return fail(r, "Outputs exceed inputs: invalid transaction.");
  r.fee = r.inputTotal - r.outputTotal;
  // Weight after signing: P2WPKH inputs are 41 bytes + a witness of at most
  // 108 (item count, 72-byte signature, 33-byte key, length bytes).
  uint64_t base = 4 + varintSize(psbt.inputCount) + 41ull * psbt.inputCount + varintSize(psbt.outputCount) + outputBytes + 4;
  uint64_t weight = base * 4 + 2 + 108ull * psbt.inputCount;
  r.vsize = (uint32_t)((weight + 3) / 4);
  r.feeRateX10 = (uint32_t)(r.fee * 10 / r.vsize);
  if (r.fee > 50000) {
    r.highFee = true;
    strcpy(r.highFeeWhy, "over 50,000 sats");
  } else if (r.feeRateX10 > 5000) {
    r.highFee = true;
    strcpy(r.highFeeWhy, "over 500 sat/vB");
  } else if (r.fee * 100 > r.sentTotal) {
    r.highFee = true;
    strcpy(r.highFeeWhy, "over 1% of the amount sent");
  }
}
