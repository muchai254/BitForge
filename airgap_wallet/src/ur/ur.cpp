// See ur.h. Ported from bc-ur (bytewords.cpp, xoshiro256.cpp,
// random-sampler.cpp, fountain-utils.cpp, fountain-decoder.cpp,
// ur-decoder.cpp); the decoder's bookkeeping is rewritten for fixed memory.
#include "ur.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

namespace ur {

// CRC-32/ISO-HDLC (zlib): reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF.
uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  }
  return crc ^ 0xFFFFFFFF;
}

static const char BYTEWORDS[] =
    "ableacidalsoapexaquaarchatomauntawayaxisbackbaldbarnbeltbetabiasbluebodybragbrewbulbbuzzcalmcashcatschefcityclaw"
    "codecolacookcostcruxcurlcuspcyandarkdatadaysdelidicedietdoordowndrawdropdrumdulldutyeacheasyechoedgeepicevenexam"
    "exiteyesfactfairfernfigsfilmfishfizzflapflewfluxfoxyfreefrogfuelfundgalagamegeargemsgiftgirlglowgoodgraygrimguru"
    "gushgyrohalfhanghardhawkheathelphighhillholyhopehornhutsicedideaidleinchinkyintoirisironitemjadejazzjoinjoltjowl"
    "judojugsjumpjunkjurykeepkenokeptkeyskickkilnkingkitekiwiknoblamblavalazyleaflegsliarlimplionlistlogoloudloveluau"
    "lucklungmainmanymathmazememomenumeowmildmintmissmonknailnavyneednewsnextnoonnotenumbobeyoboeomitonyxopenovalowls"
    "paidpartpeckplaypluspoempoolposepuffpumapurrquadquizraceramprealredorichroadrockroofrubyruinrunsrustsafesagascar"
    "setssilkskewslotsoapsolosongstubsurfswantacotasktaxitenttiedtimetinytoiltombtoystriptunatwinuglyundouniturgeuser"
    "vastveryvetovialvibeviewvisavoidvowswallwandwarmwaspwavewaxywebswhatwhenwhizwolfworkyankyawnyellyogayurtzapszero"
    "zestzinczonezoom";
static_assert(sizeof(BYTEWORDS) == 256 * 4 + 1, "bytewords table");

// First + last letter of each word -> byte (-1 = no such word).
static int16_t minimalLut[26 * 26];
static bool lutReady = false;
static void buildLut() {
  if (lutReady) return;
  for (int i = 0; i < 26 * 26; i++) minimalLut[i] = -1;
  for (int b = 0; b < 256; b++) minimalLut[(BYTEWORDS[b * 4] - 'a') * 26 + (BYTEWORDS[b * 4 + 3] - 'a')] = b;
  lutReady = true;
}
static int lowerLetter(char c) {
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= 'A' && c <= 'Z') return c - 'A';
  return -1;
}

int bytewordsDecodeMinimal(const char *s, size_t len, uint8_t *out, size_t outCap) {
  buildLut();
  if (len % 2) return -1;
  size_t n = len / 2;
  if (n < 5 || n - 4 > outCap) return -1;
  uint8_t crcBytes[4];
  for (size_t i = 0; i < n; i++) {
    int a = lowerLetter(s[2 * i]), b = lowerLetter(s[2 * i + 1]);
    if (a < 0 || b < 0) return -1;
    int v = minimalLut[a * 26 + b];
    if (v < 0) return -1;
    if (i < n - 4) out[i] = (uint8_t)v;
    else crcBytes[i - (n - 4)] = (uint8_t)v;
  }
  uint32_t want = crc32(out, n - 4);
  uint32_t got = ((uint32_t)crcBytes[0] << 24) | ((uint32_t)crcBytes[1] << 16) | ((uint32_t)crcBytes[2] << 8) | crcBytes[3];
  return want == got ? (int)(n - 4) : -1;
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

Xoshiro256::Xoshiro256(const uint8_t *seed, size_t len) {
  uint8_t d[32];
  mbedtls_sha256(seed, len, d, 0);
  for (int i = 0; i < 4; i++) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v = (v << 8) | d[i * 8 + k];
    s_[i] = v;
  }
}
uint64_t Xoshiro256::next() {
  const uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}
// As bc-ur: next() / ((double)UINT64_MAX + 1). The uint64 -> double
// conversion rounds to nearest (software double on the ESP32, IEEE-exact).
double Xoshiro256::nextDouble() { return (double)next() / 18446744073709551616.0; }
uint32_t Xoshiro256::nextInt(uint32_t low, uint32_t high) {
  return (uint32_t)(nextDouble() * (double)((uint64_t)high - low + 1)) + low;
}

// Vose's alias method, as in random-sampler.cpp.
void DegreeSampler::release() {
  free(probs_);
  free(aliases_);
  probs_ = nullptr;
  aliases_ = nullptr;
  n_ = 0;
}
bool DegreeSampler::init(uint32_t n) {
  if (n == n_ && probs_) return true;
  release();
  if (n == 0) return false;
  probs_ = (double *)malloc(n * sizeof(double));
  aliases_ = (uint32_t *)calloc(n, sizeof(uint32_t));
  double *P = (double *)malloc(n * sizeof(double));
  uint32_t *S = (uint32_t *)malloc(n * sizeof(uint32_t));
  uint32_t *L = (uint32_t *)malloc(n * sizeof(uint32_t));
  if (!probs_ || !aliases_ || !P || !S || !L) {
    free(P); free(S); free(L);
    release();
    return false;
  }
  double sum = 0.0;
  for (uint32_t i = 0; i < n; i++) sum += 1.0 / (double)(i + 1);
  for (uint32_t i = 0; i < n; i++) P[i] = (1.0 / (double)(i + 1)) * (double)n / sum;
  uint32_t ns = 0, nl = 0;
  for (int64_t i = (int64_t)n - 1; i >= 0; i--) {
    if (P[i] < 1.0) S[ns++] = (uint32_t)i;
    else L[nl++] = (uint32_t)i;
  }
  for (uint32_t i = 0; i < n; i++) probs_[i] = 0.0;
  while (ns > 0 && nl > 0) {
    uint32_t a = S[--ns], g = L[--nl];
    probs_[a] = P[a];
    aliases_[a] = g;
    P[g] += P[a] - 1.0;
    if (P[g] < 1.0) S[ns++] = g;
    else L[nl++] = g;
  }
  while (nl > 0) probs_[L[--nl]] = 1.0;
  while (ns > 0) probs_[S[--ns]] = 1.0;
  free(P); free(S); free(L);
  n_ = n;
  return true;
}
uint32_t DegreeSampler::next(Xoshiro256 &rng) {
  double r1 = rng.nextDouble(), r2 = rng.nextDouble();
  uint32_t i = (uint32_t)((double)n_ * r1);
  return r2 < probs_[i] ? i : aliases_[i];
}

static inline void setBit(uint8_t *b, uint32_t i) { b[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline bool getBit(const uint8_t *b, uint32_t i) { return (b[i >> 3] >> (i & 7)) & 1; }
static inline void clearBit(uint8_t *b, uint32_t i) { b[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

uint32_t chooseFragments(uint32_t seqNum, uint32_t seqLen, uint32_t checksum, DegreeSampler &sampler,
                         uint8_t *bits, uint32_t *scratch) {
  memset(bits, 0, (seqLen + 7) / 8);
  if (seqNum <= seqLen) {
    setBit(bits, seqNum - 1);
    return 1;
  }
  uint8_t seed[8] = {(uint8_t)(seqNum >> 24), (uint8_t)(seqNum >> 16), (uint8_t)(seqNum >> 8), (uint8_t)seqNum,
                     (uint8_t)(checksum >> 24), (uint8_t)(checksum >> 16), (uint8_t)(checksum >> 8), (uint8_t)checksum};
  Xoshiro256 rng(seed, sizeof seed);
  uint32_t degree = sampler.next(rng) + 1;
  // bc-ur shuffles all indexes and keeps the first `degree`; the rng isn't
  // used afterwards, so stopping after `degree` picks gives the same set.
  uint32_t remaining = seqLen;
  for (uint32_t i = 0; i < seqLen; i++) scratch[i] = i;
  for (uint32_t k = 0; k < degree; k++) {
    uint32_t index = rng.nextInt(0, remaining - 1);
    setBit(bits, scratch[index]);
    memmove(&scratch[index], &scratch[index + 1], (remaining - index - 1) * sizeof(uint32_t));
    remaining--;
  }
  return degree;
}

// Reads one CBOR head at buf[*pos]. Only what a part needs: values up to 32 bits.
static bool cborHead(const uint8_t *buf, size_t len, size_t *pos, uint8_t *major, uint32_t *value) {
  if (*pos >= len) return false;
  uint8_t ib = buf[(*pos)++];
  *major = ib >> 5;
  uint8_t info = ib & 31;
  int extra = info < 24 ? 0 : info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : -1;
  if (extra < 0 || len - *pos < (size_t)extra) return false;
  uint32_t v = info < 24 ? info : 0;
  for (int i = 0; i < extra; i++) v = (v << 8) | buf[(*pos)++];
  *value = v;
  return true;
}

bool decodePart(const uint8_t *cbor, size_t len, Part &out) {
  size_t pos = 0;
  uint8_t major;
  uint32_t v, f[4];
  if (!cborHead(cbor, len, &pos, &major, &v) || major != 4 || v != 5) return false;
  for (int i = 0; i < 4; i++) {
    if (!cborHead(cbor, len, &pos, &major, &f[i]) || major != 0) return false;
  }
  if (!cborHead(cbor, len, &pos, &major, &v) || major != 2) return false;
  if (len - pos != v) return false; // also refuses trailing bytes
  out.seqNum = f[0];
  out.seqLen = f[1];
  out.messageLen = f[2];
  out.checksum = f[3];
  out.data = cbor + pos;
  out.dataLen = v;
  return true;
}

bool unwrapByteString(const uint8_t *cbor, size_t len, const uint8_t **out, size_t *outLen) {
  size_t pos = 0;
  uint8_t major;
  uint32_t v;
  if (!cborHead(cbor, len, &pos, &major, &v) || major != 2 || len - pos != v) return false;
  *out = cbor + pos;
  *outLen = v;
  return true;
}

const char *statusName(Status s) {
  switch (s) {
    case ACCEPTED: return "accepted";
    case DUPLICATE: return "duplicate";
    case COMPLETE: return "complete";
    case NOT_UR: return "not a UR";
    case INVALID: return "invalid";
    case WRONG_TYPE: return "wrong type";
    case OTHER_MESSAGE: return "other message";
    case TOO_LARGE: return "too large";
    case NO_MEMORY: return "out of memory";
    case FAILED: return "checksum failed";
    case ALREADY_DONE: return "already done";
  }
  return "?";
}

// Mixed parts may use at most this much heap, whatever the fragment size.
static const size_t MIXED_BUDGET = 32 * 1024;
static const size_t PART_BUF = MAX_FRAGMENT + 32; // fragment + CBOR header

static void zfree(void *p, size_t n) {
  if (p) {
    mbedtls_platform_zeroize(p, n);
    free(p);
  }
}

Decoder::Decoder(const char *const *acceptedTypes) : accepted_types_(acceptedTypes) {}
Decoder::~Decoder() { reset(); }

// Frees what's only needed while assembling (heap for the PSBT parser).
void Decoder::releaseWork() {
  size_t bitsBytes = (seqLen_ + 7) / 8;
  zfree(scratch_, seqLen_ * sizeof(uint32_t));
  zfree(queue_, seqLen_ * sizeof(uint32_t));
  zfree(partBuf_, PART_BUF);
  zfree(tmpBits_, bitsBytes);
  zfree(tmpData_, fragLen_);
  zfree(pool_, poolSize_);
  scratch_ = queue_ = nullptr;
  partBuf_ = tmpBits_ = tmpData_ = pool_ = nullptr;
  poolSize_ = 0;
  mixedCount_ = maxMixed_ = 0;
  qHead_ = qTail_ = 0;
  sampler_.release();
}

void Decoder::reset() {
  releaseWork();
  zfree(msg_, msgCap_);
  zfree(knownBits_, (seqLen_ + 7) / 8);
  msg_ = knownBits_ = nullptr;
  type_[0] = 0;
  started_ = done_ = failed_ = false;
  seqLen_ = messageLen_ = checksum_ = fragLen_ = 0;
  msgCap_ = 0;
  known_ = accepted_ = 0;
}

static bool parseU32(const char *s, size_t n, uint32_t *out) {
  if (n == 0 || n > 10) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (uint64_t)(s[i] - '0');
  }
  if (v > 0xFFFFFFFFull) return false;
  *out = (uint32_t)v;
  return true;
}

Status Decoder::receive(const char *text, size_t len) {
  if (done_) return ALREADY_DONE;
  if (len < 3 || lowerLetter(text[0]) != 'u' - 'a' || lowerLetter(text[1]) != 'r' - 'a' || text[2] != ':') return NOT_UR;
  // Components: type / [seq /] body.
  const char *p = text + 3, *end = text + len;
  const char *slash1 = (const char *)memchr(p, '/', end - p);
  if (!slash1) return INVALID;
  const char *slash2 = (const char *)memchr(slash1 + 1, '/', end - slash1 - 1);
  if (slash2 && memchr(slash2 + 1, '/', end - slash2 - 1)) return INVALID;

  size_t typeLen = slash1 - p;
  if (typeLen == 0 || typeLen > MAX_TYPE) return INVALID;
  char type[MAX_TYPE + 1];
  for (size_t i = 0; i < typeLen; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return INVALID;
    type[i] = c;
  }
  type[typeLen] = 0;
  if (accepted_types_) {
    bool ok = false;
    for (const char *const *t = accepted_types_; *t; t++) ok = ok || strcmp(*t, type) == 0;
    if (!ok) return WRONG_TYPE;
  }
  if (started_ && strcmp(type, type_) != 0) return WRONG_TYPE;

  if (!slash2) {
    // Single-part UR: the body is the whole message.
    if (started_) return OTHER_MESSAGE;
    const char *body = slash1 + 1;
    size_t bodyLen = end - body;
    if (bodyLen / 2 < 5) return INVALID;
    size_t cap = bodyLen / 2 - 4;
    if (cap > MAX_MESSAGE) return TOO_LARGE;
    uint8_t *m = (uint8_t *)malloc(cap);
    if (!m) return NO_MEMORY;
    int n = bytewordsDecodeMinimal(body, bodyLen, m, cap);
    if (n < 0) {
      zfree(m, cap);
      return INVALID;
    }
    msg_ = m;
    msgCap_ = cap;
    messageLen_ = (uint32_t)n;
    memcpy(type_, type, typeLen + 1);
    started_ = done_ = true;
    accepted_ = 1;
    return COMPLETE;
  }

  // Multipart: "<seqNum>-<seqLen>".
  const char *seq = slash1 + 1;
  const char *dash = (const char *)memchr(seq, '-', slash2 - seq);
  uint32_t seqNum, seqLen;
  if (!dash || !parseU32(seq, dash - seq, &seqNum) || !parseU32(dash + 1, slash2 - dash - 1, &seqLen)) return INVALID;
  const char *body = slash2 + 1;
  size_t bodyLen = end - body;
  if (bodyLen / 2 > PART_BUF + 4) return TOO_LARGE;
  if (!partBuf_ && !(partBuf_ = (uint8_t *)malloc(PART_BUF))) return NO_MEMORY;
  int n = bytewordsDecodeMinimal(body, bodyLen, partBuf_, PART_BUF);
  if (n < 0) return INVALID;
  Part part;
  if (!decodePart(partBuf_, (size_t)n, part)) return INVALID;
  if (part.seqNum != seqNum || part.seqLen != seqLen) return INVALID;
  if (part.seqNum == 0 || part.seqLen == 0 || part.dataLen == 0 || part.messageLen == 0) return INVALID;
  if (part.seqLen > MAX_SEQ_LEN || part.dataLen > MAX_FRAGMENT || part.messageLen > MAX_MESSAGE) return TOO_LARGE;
  // The encoder splits the message into ceil(messageLen / fragmentLen)
  // fragments; anything else is malformed.
  if ((part.messageLen + part.dataLen - 1) / part.dataLen != part.seqLen) return INVALID;

  if (!started_) {
    Status st = start(type, part);
    if (st != ACCEPTED) return st;
  } else if (part.seqLen != seqLen_ || part.messageLen != messageLen_ || part.checksum != checksum_ ||
             part.dataLen != fragLen_) {
    return OTHER_MESSAGE;
  }
  return addPart(part);
}

Status Decoder::start(const char *type, const Part &p) {
  seqLen_ = p.seqLen;
  messageLen_ = p.messageLen;
  checksum_ = p.checksum;
  fragLen_ = (uint32_t)p.dataLen;
  size_t bitsBytes = (seqLen_ + 7) / 8;
  msgCap_ = (size_t)seqLen_ * fragLen_; // < messageLen + fragLen, by the check above
  maxMixed_ = (int)(MIXED_BUDGET / (fragLen_ + bitsBytes));
  if (maxMixed_ > MAX_MIXED) maxMixed_ = MAX_MIXED;
  if (maxMixed_ < 4) maxMixed_ = 4;
  poolSize_ = (size_t)maxMixed_ * (bitsBytes + fragLen_);
  msg_ = (uint8_t *)calloc(1, msgCap_);
  knownBits_ = (uint8_t *)calloc(1, bitsBytes);
  scratch_ = (uint32_t *)malloc(seqLen_ * sizeof(uint32_t));
  queue_ = (uint32_t *)malloc(seqLen_ * sizeof(uint32_t));
  tmpBits_ = (uint8_t *)malloc(bitsBytes);
  tmpData_ = (uint8_t *)malloc(fragLen_);
  pool_ = (uint8_t *)malloc(poolSize_);
  if (!msg_ || !knownBits_ || !scratch_ || !queue_ || !tmpBits_ || !tmpData_ || !pool_ || !sampler_.init(seqLen_)) {
    reset();
    return NO_MEMORY;
  }
  for (int i = 0; i < maxMixed_; i++) {
    mixed_[i].bits = pool_ + (size_t)i * (bitsBytes + fragLen_);
    mixed_[i].data = mixed_[i].bits + bitsBytes;
    mixed_[i].count = 0;
  }
  memcpy(type_, type, strlen(type) + 1);
  started_ = true;
  return ACCEPTED;
}

static void xorInto(uint8_t *dst, const uint8_t *src, size_t n) {
  for (size_t i = 0; i < n; i++) dst[i] ^= src[i];
}
static bool strictSubset(const uint8_t *a, uint32_t ca, const uint8_t *b, uint32_t cb, size_t bytes) {
  if (ca >= cb) return false;
  for (size_t i = 0; i < bytes; i++)
    if (a[i] & ~b[i]) return false;
  return true;
}
static uint32_t firstBit(const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (getBit(b, i)) return i;
  return n;
}

// XORs out every fragment of `bits` that's already known.
void Decoder::reduceByKnown(uint8_t *bits, uint8_t *data, uint32_t &count) {
  size_t bitsBytes = (seqLen_ + 7) / 8;
  for (size_t byte = 0; byte < bitsBytes; byte++) {
    uint8_t both = bits[byte] & knownBits_[byte];
    if (!both) continue;
    for (int k = 0; k < 8; k++) {
      if (!(both >> k & 1)) continue;
      uint32_t i = (uint32_t)(byte * 8 + k);
      xorInto(data, msg_ + (size_t)i * fragLen_, fragLen_);
      clearBit(bits, i);
      count--;
    }
  }
}

// Swaps the last slot into i (slots are pointers into the pool). Callers
// iterate backwards, so the slot moved into i has already been visited.
void Decoder::dropMixed(int i) {
  Mixed t = mixed_[i];
  mixed_[i] = mixed_[mixedCount_ - 1];
  mixed_[mixedCount_ - 1] = t;
  mixedCount_--;
}

// Stores fragment `index` (if new) and queues it for propagate().
void Decoder::placeKnown(uint32_t index, const uint8_t *data) {
  if (getBit(knownBits_, index)) return;
  memcpy(msg_ + (size_t)index * fragLen_, data, fragLen_);
  setBit(knownBits_, index);
  known_++;
  queue_[qTail_++] = index; // each fragment is queued at most once
}

// Reduces every mixed part by the queued fragments, and by any fragment that
// recovers in turn.
Status Decoder::propagate() {
  while (qHead_ < qTail_) {
    uint32_t f = queue_[qHead_++];
    for (int i = mixedCount_ - 1; i >= 0; i--) {
      Mixed &m = mixed_[i];
      if (!getBit(m.bits, f)) continue;
      xorInto(m.data, msg_ + (size_t)f * fragLen_, fragLen_);
      clearBit(m.bits, f);
      m.count--;
      if (m.count == 1) placeKnown(firstBit(m.bits, seqLen_), m.data);
      if (m.count <= 1) dropMixed(i);
    }
  }
  return known_ == seqLen_ ? finish() : ACCEPTED;
}

Status Decoder::addPart(const Part &p) {
  size_t bitsBytes = (seqLen_ + 7) / 8;
  uint32_t count = chooseFragments(p.seqNum, seqLen_, checksum_, sampler_, tmpBits_, scratch_);
  memcpy(tmpData_, p.data, fragLen_);
  reduceByKnown(tmpBits_, tmpData_, count);
  if (count == 0) return DUPLICATE;
  // Reduce by stored mixed parts that are strict subsets of this one.
  for (int i = 0; i < mixedCount_ && count > 1; i++) {
    Mixed &m = mixed_[i];
    if (strictSubset(m.bits, m.count, tmpBits_, count, bitsBytes)) {
      xorInto(tmpData_, m.data, fragLen_);
      for (size_t b = 0; b < bitsBytes; b++) tmpBits_[b] ^= m.bits[b];
      count -= m.count;
    }
  }
  if (count > 1) {
    for (int i = 0; i < mixedCount_; i++)
      if (mixed_[i].count == count && memcmp(mixed_[i].bits, tmpBits_, bitsBytes) == 0) return DUPLICATE;
  }
  accepted_++;
  if (count == 1) {
    placeKnown(firstBit(tmpBits_, seqLen_), tmpData_);
    return propagate();
  }
  // Reduce stored mixed parts that contain this one.
  for (int i = mixedCount_ - 1; i >= 0; i--) {
    Mixed &m = mixed_[i];
    if (!strictSubset(tmpBits_, count, m.bits, m.count, bitsBytes)) continue;
    xorInto(m.data, tmpData_, fragLen_);
    for (size_t b = 0; b < bitsBytes; b++) m.bits[b] ^= tmpBits_[b];
    m.count -= count;
    if (m.count == 1) {
      placeKnown(firstBit(m.bits, seqLen_), m.data);
      dropMixed(i);
    }
  }
  // Keep this part, evicting the stored part with the most fragments if full
  // (losing one only means waiting for more frames).
  int slot = -1;
  if (mixedCount_ < maxMixed_) {
    slot = mixedCount_++;
  } else {
    int worst = 0;
    for (int i = 1; i < mixedCount_; i++)
      if (mixed_[i].count > mixed_[worst].count) worst = i;
    if (mixed_[worst].count > count) slot = worst;
  }
  if (slot >= 0) {
    memcpy(mixed_[slot].bits, tmpBits_, bitsBytes);
    memcpy(mixed_[slot].data, tmpData_, fragLen_);
    mixed_[slot].count = count;
  }
  return propagate();
}

Status Decoder::finish() {
  done_ = true;
  failed_ = crc32(msg_, messageLen_) != checksum_;
  releaseWork();
  return failed_ ? FAILED : COMPLETE;
}

int bytewordsEncodeMinimal(const uint8_t *data, size_t len, char *out, size_t outCap) {
  if ((len + 4) * 2 + 1 > outCap) return -1;
  uint32_t c = crc32(data, len);
  const uint8_t tail[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
  size_t o = 0;
  for (size_t i = 0; i < len + 4; i++) {
    uint8_t b = i < len ? data[i] : tail[i - len];
    out[o++] = BYTEWORDS[b * 4];
    out[o++] = BYTEWORDS[b * 4 + 3];
  }
  out[o] = 0;
  return (int)o;
}

// Writes a CBOR head with the shortest length; returns the bytes written.
static size_t putCborHead(uint8_t *p, uint8_t major, uint32_t v) {
  uint8_t m = (uint8_t)(major << 5);
  if (v < 24) {
    p[0] = m | (uint8_t)v;
    return 1;
  }
  if (v < 0x100) {
    p[0] = m | 24;
    p[1] = (uint8_t)v;
    return 2;
  }
  if (v < 0x10000) {
    p[0] = m | 25;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
    return 3;
  }
  p[0] = m | 26;
  p[1] = (uint8_t)(v >> 24);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 8);
  p[4] = (uint8_t)v;
  return 5;
}

size_t wrapByteString(const uint8_t *data, size_t len, uint8_t *out) {
  size_t h = putCborHead(out, 2, (uint32_t)len);
  memcpy(out + h, data, len);
  return h + len;
}

void Encoder::reset() {
  size_t bitsBytes = (seqLen_ + 7) / 8;
  zfree(msg_, (size_t)seqLen_ * fragLen_);
  zfree(bits_, bitsBytes);
  zfree(scratch_, seqLen_ * sizeof(uint32_t));
  zfree(cbor_, (size_t)fragLen_ + 32);
  sampler_.release();
  msg_ = bits_ = cbor_ = nullptr;
  scratch_ = nullptr;
  seqLen_ = fragLen_ = seqNum_ = messageLen_ = checksum_ = 0;
  type_ = nullptr;
}

bool Encoder::begin(const char *type, const uint8_t *message, size_t len, uint32_t maxFragment, uint32_t minFragment,
                    uint32_t firstSeqNum) {
  reset();
  if (len == 0 || len > MAX_MESSAGE || maxFragment == 0 || minFragment == 0) return false;
  // bc-ur's find_nominal_fragment_length: the fewest fragments that fit.
  size_t fragLen = len;
  size_t maxCount = len / minFragment;
  for (size_t count = 1; count <= maxCount; count++) {
    fragLen = (len + count - 1) / count;
    if (fragLen <= maxFragment) break;
  }
  type_ = type;
  fragLen_ = (uint32_t)fragLen;
  seqLen_ = (uint32_t)((len + fragLen - 1) / fragLen);
  messageLen_ = (uint32_t)len;
  checksum_ = crc32(message, len);
  seqNum_ = firstSeqNum;
  msg_ = (uint8_t *)calloc(seqLen_, fragLen_); // the last fragment is zero-padded
  bits_ = (uint8_t *)malloc((seqLen_ + 7) / 8);
  scratch_ = (uint32_t *)malloc(seqLen_ * sizeof(uint32_t));
  cbor_ = (uint8_t *)malloc((size_t)fragLen_ + 32);
  if (!msg_ || !bits_ || !scratch_ || !cbor_ || !sampler_.init(seqLen_)) {
    reset();
    return false;
  }
  memcpy(msg_, message, len);
  return true;
}

// Writes "ur:<type>/<seq>-<len>/<body>", or "ur:<type>/<body>" when the
// message fits one fragment.
int Encoder::nextPart(char *out, size_t cap) {
  if (!msg_) return -1;
  int n;
  if (seqLen_ == 1) {
    n = snprintf(out, cap, "ur:%s/", type_);
    if (n < 0 || (size_t)n >= cap) return -1;
    int b = bytewordsEncodeMinimal(msg_, messageLen_, out + n, cap - n);
    return b < 0 ? -1 : n + b;
  }
  seqNum_++;
  size_t p = putCborHead(cbor_, 4, 5); // heads: at most 1 + 4 * 5 + 5 bytes
  p += putCborHead(cbor_ + p, 0, seqNum_);
  p += putCborHead(cbor_ + p, 0, seqLen_);
  p += putCborHead(cbor_ + p, 0, messageLen_);
  p += putCborHead(cbor_ + p, 0, checksum_);
  p += putCborHead(cbor_ + p, 2, fragLen_);
  memset(cbor_ + p, 0, fragLen_);
  chooseFragments(seqNum_, seqLen_, checksum_, sampler_, bits_, scratch_);
  for (uint32_t i = 0; i < seqLen_; i++) {
    if (getBit(bits_, i)) xorInto(cbor_ + p, msg_ + (size_t)i * fragLen_, fragLen_);
  }
  p += fragLen_;
  n = snprintf(out, cap, "ur:%s/%lu-%lu/", type_, (unsigned long)seqNum_, (unsigned long)seqLen_);
  if (n < 0 || (size_t)n >= cap) return -1;
  int b = bytewordsEncodeMinimal(cbor_, p, out + n, cap - n);
  return b < 0 ? -1 : n + b;
}

} // namespace ur
