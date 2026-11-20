// Uniform Resources (BCR-2020-005) + fountain codes (BCR-2020-006) decoder
// for BitForge: reads Sparrow's animated "ur:crypto-psbt/..." QRs.
// Same algorithms as Blockchain Commons' bc-ur C++ library (BSD-2-Clause-
// Plus-Patent, see LICENSE-bc-ur.txt), rewritten for a small, hostile-input
// setting: no exceptions, no STL, fixed limits, every length checked.
// tools/ur/ur.js is the PC reference; both pass the upstream test vectors.
//
// Camera input is hostile: anything that doesn't parse, doesn't match the
// first part, or exceeds a limit is refused without changing decoder state.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ur {

// Prototype limits (PSBTs over 64 KB are refused).
constexpr size_t MAX_MESSAGE = 65536;  // CBOR message bytes
constexpr uint32_t MAX_SEQ_LEN = 1024; // fragments per message
constexpr size_t MAX_FRAGMENT = 2048;  // bytes per fragment (a QR holds < 3 KB)
constexpr int MAX_MIXED = 64;          // mixed parts kept for later reduction
constexpr size_t MAX_TYPE = 24;

uint32_t crc32(const uint8_t *data, size_t len);

// Decodes minimal bytewords (2 letters per byte, case-insensitive), checks the
// trailing CRC-32 and writes the body without it. Returns its length or -1.
int bytewordsDecodeMinimal(const char *s, size_t len, uint8_t *out, size_t outCap);

// Encodes data + its CRC-32 as minimal bytewords, NUL-terminated. Returns the
// length or -1.
int bytewordsEncodeMinimal(const uint8_t *data, size_t len, char *out, size_t outCap);

// Wraps data as one CBOR byte string (out needs len + 5 bytes). Returns the
// total length.
size_t wrapByteString(const uint8_t *data, size_t len, uint8_t *out);

// Xoshiro256**, seeded from SHA-256 of the seed bytes (as bc-ur does).
class Xoshiro256 {
 public:
  Xoshiro256(const uint8_t *seed, size_t len);
  uint64_t next();
  double nextDouble(); // [0, 1)
  uint32_t nextInt(uint32_t low, uint32_t high); // inclusive
 private:
  uint64_t s_[4];
};

// Vose's alias sampler over the degree distribution 1/1, 1/2, ... 1/n.
class DegreeSampler {
 public:
  DegreeSampler() : n_(0), probs_(nullptr), aliases_(nullptr) {}
  ~DegreeSampler() { release(); }
  bool init(uint32_t n); // false on allocation failure
  uint32_t next(Xoshiro256 &rng); // 0-based
  void release();
 private:
  uint32_t n_;
  double *probs_;
  uint32_t *aliases_;
};

// Picks the fragment indexes mixed into part seqNum (bc-ur's
// choose_fragments) and sets their bits in `bits` (seqLen bits, cleared
// first). Returns the number of indexes. `sampler` must be init(seqLen).
// `scratch` needs seqLen entries.
uint32_t chooseFragments(uint32_t seqNum, uint32_t seqLen, uint32_t checksum, DegreeSampler &sampler,
                         uint8_t *bits, uint32_t *scratch);

struct Part {
  uint32_t seqNum, seqLen, messageLen, checksum;
  const uint8_t *data; // points into the caller's buffer
  size_t dataLen;
};
// Parses the CBOR [seqNum, seqLen, messageLen, checksum, data]. False if
// it's anything else, including trailing bytes.
bool decodePart(const uint8_t *cbor, size_t len, Part &out);

// The CBOR payload of ur:crypto-psbt / ur:psbt / ur:bytes is one byte string:
// finds it. False if the message is anything else.
bool unwrapByteString(const uint8_t *cbor, size_t len, const uint8_t **out, size_t *outLen);

enum Status {
  ACCEPTED,    // new information (or a repeat of a part we couldn't use yet)
  DUPLICATE,   // nothing new
  COMPLETE,    // this part completed the message
  NOT_UR,
  INVALID,     // looks like a UR but is malformed (bad bytewords, CRC, CBOR...)
  WRONG_TYPE,  // a UR type we don't accept, or not the type of the first part
  OTHER_MESSAGE, // a part of a different message (other length/checksum)
  TOO_LARGE,
  NO_MEMORY,
  FAILED,      // all fragments arrived but the message checksum is wrong
  ALREADY_DONE,
};
const char *statusName(Status s);

// Assembles one UR message from its parts. Feed it every scanned string;
// it ignores repeats. Hold at most one at a time: a 64 KB message needs
// ~64 KB of heap plus the mixed parts.
class Decoder {
 public:
  // acceptedTypes: lower-case UR types, nullptr-terminated (nullptr = any).
  explicit Decoder(const char *const *acceptedTypes = nullptr);
  ~Decoder();
  Status receive(const char *text, size_t len);
  bool isComplete() const { return done_ && !failed_; }
  bool isFailed() const { return failed_; }
  const uint8_t *message() const { return isComplete() ? msg_ : nullptr; }
  size_t messageLen() const { return isComplete() ? messageLen_ : 0; }
  const char *type() const { return type_; }
  // Progress: fragments recovered so far out of seqLen (0/0 before the
  // first multipart frame). A single-part UR goes straight to COMPLETE.
  uint32_t fragmentsKnown() const { return known_; }
  uint32_t seqLen() const { return seqLen_; }
  uint32_t partsAccepted() const { return accepted_; }
  // Zeroes and frees everything; ready for a new message.
  void reset();

  // Mixed-part slots the memory budget allows for this message.
  int maxMixed() const { return maxMixed_; }

 private:
  struct Mixed {
    uint8_t *bits;  // seqLen_ bits
    uint8_t *data;  // fragLen_ bytes
    uint32_t count; // bits set (always >= 2 while stored)
  };
  Status start(const char *type, const Part &p);
  Status addPart(const Part &p);
  void placeKnown(uint32_t index, const uint8_t *data);
  Status propagate();
  void reduceByKnown(uint8_t *bits, uint8_t *data, uint32_t &count);
  void dropMixed(int i);
  Status finish();
  void releaseWork();

  const char *const *accepted_types_ = nullptr;
  char type_[MAX_TYPE + 1] = {0};
  bool started_ = false, done_ = false, failed_ = false;
  uint32_t seqLen_ = 0, messageLen_ = 0, checksum_ = 0, fragLen_ = 0;
  uint8_t *msg_ = nullptr;       // seqLen_ * fragLen_: fragment i at i * fragLen_
  size_t msgCap_ = 0;
  uint8_t *knownBits_ = nullptr; // which fragments are in msg_
  uint32_t known_ = 0, accepted_ = 0;
  Mixed mixed_[MAX_MIXED] = {};
  int mixedCount_ = 0, maxMixed_ = 0;
  uint8_t *pool_ = nullptr;      // maxMixed_ * (bits + data)
  size_t poolSize_ = 0;
  DegreeSampler sampler_;
  uint32_t *scratch_ = nullptr;  // seqLen_ entries, for chooseFragments
  uint8_t *partBuf_ = nullptr;   // decoded bytewords of the current part
  uint8_t *tmpBits_ = nullptr;   // seqLen_ bits
  uint8_t *tmpData_ = nullptr;   // fragLen_ bytes
  uint32_t *queue_ = nullptr;    // seqLen_ entries: recovered fragments to propagate
  uint32_t qHead_ = 0, qTail_ = 0;
};

// Splits a CBOR message into an endless stream of UR strings (bc-ur's
// fountain encoder): the plain fragments in order, then mixed ones, so a
// reader that missed some can still finish. Used for the signed PSBT.
class Encoder {
 public:
  ~Encoder() { reset(); }
  // type must outlive the encoder. False on bad arguments or no memory.
  bool begin(const char *type, const uint8_t *message, size_t len, uint32_t maxFragment, uint32_t minFragment = 10,
             uint32_t firstSeqNum = 0);
  // Writes the next string (lower case, NUL-terminated). Returns its length,
  // or -1 if out is too small.
  int nextPart(char *out, size_t cap);
  uint32_t seqLen() const { return seqLen_; }
  uint32_t seqNum() const { return seqNum_; }
  void reset();

 private:
  const char *type_ = nullptr;
  uint32_t seqLen_ = 0, fragLen_ = 0, seqNum_ = 0, messageLen_ = 0, checksum_ = 0;
  uint8_t *msg_ = nullptr, *bits_ = nullptr, *cbor_ = nullptr;
  uint32_t *scratch_ = nullptr;
  DegreeSampler sampler_;
};

} // namespace ur
