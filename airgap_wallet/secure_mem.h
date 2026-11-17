// Wiping secrets from RAM.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/platform_util.h>

// Zeroes n bytes in a way the compiler can't optimise away.
inline void secure_zero(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }

// Overwrites the calling task's unused stack (everything below the caller's
// frame, except a small margin at the very bottom where FreeRTOS keeps its
// overflow canary). Call it right after key work returns: whatever secrets
// library code left in its stack frames are then gone. Fills with FreeRTOS's
// stack fill byte (0xA5), so uxTaskGetStackHighWaterMark() keeps working; it
// then reports usage since the last scrub. Returns the number of bytes wiped.
size_t secure_scrub_stack();

// Zeroes the free heap: briefly allocates every free block it can get,
// zeroes it, and frees it again. Use after freeing buffers that held secrets
// we couldn't wipe ourselves (e.g. a driver's receive buffer). Returns the
// number of bytes zeroed. Takes a few ms; nothing else can allocate while it
// runs.
size_t secure_scrub_free_heap();

// Fixed-capacity buffer for secret bytes or text (mnemonic, passphrase, seed).
// Zeroes itself when it goes out of scope. It can't be copied, so a secret
// never gets duplicated by accident; pass it by reference.
template <size_t N>
class SecretBuffer {
 public:
  SecretBuffer() { clear(); }
  ~SecretBuffer() { clear(); }
  SecretBuffer(const SecretBuffer &) = delete;
  SecretBuffer &operator=(const SecretBuffer &) = delete;

  void clear() {
    secure_zero(buf_, sizeof buf_);
    len_ = 0;
  }
  uint8_t *data() { return buf_; }
  const uint8_t *data() const { return buf_; }
  // For text: data is always followed by a '\0' (one byte is reserved for it).
  char *c_str() { return reinterpret_cast<char *>(buf_); }
  const char *c_str() const { return reinterpret_cast<const char *>(buf_); }
  size_t size() const { return len_; }
  static constexpr size_t capacity() { return N - 1; }

  // Sets the length after writing into data(). Returns false (and clears) if
  // it's too long.
  bool setSize(size_t n) {
    if (n > capacity()) {
      clear();
      return false;
    }
    len_ = n;
    buf_[n] = 0;
    return true;
  }
  // Appends bytes. Returns false (and leaves the buffer unchanged) if they don't fit.
  bool append(const void *p, size_t n) {
    if (n > capacity() - len_) return false;
    const uint8_t *s = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) buf_[len_ + i] = s[i];
    len_ += n;
    buf_[len_] = 0;
    return true;
  }

 private:
  uint8_t buf_[N];
  size_t len_;
};
