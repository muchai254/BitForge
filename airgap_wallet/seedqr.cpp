#include "seedqr.h"
#include <Bitcoin.h> // first: lets arduino-cli find uBitcoin for the trezor header below
#include <string.h>
#include <utility/trezor/bip39.h>
#include "bip39_words.h"

static bool allDigits(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
  }
  return true;
}

bool seedqrLooksLike(const uint8_t *p, size_t len) {
  if ((len == 48 || len == 96) && allDigits(p, len)) return true; // standard
  if (len == 16 || len == 32) return true;                       // compact (any bytes)
  return false;
}

size_t seedqrEncode(const char *mnemonic, SeedQrFormat fmt, SecretBuffer<97> &out) {
  out.clear();
  if (fmt == SEEDQR_COMPACT) {
    size_t n = mnemonicToEntropy(mnemonic, strlen(mnemonic), out.data(), out.capacity());
    if (n != 16 && n != 32) {
      out.clear();
      return 0;
    }
    out.setSize(n);
    return n;
  }
  if (fmt != SEEDQR_STANDARD) return 0;
  const char *p = mnemonic;
  char word[12], digits[5];
  int words = 0;
  while (*p) {
    size_t len = strcspn(p, " ");
    if (len == 0 || len >= sizeof word) break;
    memcpy(word, p, len);
    word[len] = 0;
    int idx = bip39Index(word);
    if (idx < 0) break;
    digits[0] = (char)('0' + idx / 1000); // idx is 0..2047
    digits[1] = (char)('0' + idx / 100 % 10);
    digits[2] = (char)('0' + idx / 10 % 10);
    digits[3] = (char)('0' + idx % 10);
    digits[4] = 0;
    idx = 0;
    if (!out.append(digits, 4)) break;
    words++;
    p += len;
    while (*p == ' ') p++;
  }
  secure_zero(word, sizeof word);
  secure_zero(digits, sizeof digits);
  if (*p || (words != 12 && words != 24)) {
    out.clear();
    return 0;
  }
  return out.size();
}

int seedqrDecode(const uint8_t *p, size_t len, SecretBuffer<SESSION_MNEMONIC_CAP> &mnemonic, SeedQrFormat *fmt) {
  mnemonic.clear();
  *fmt = SEEDQR_NONE;
  int words = 0;
  if ((len == 48 || len == 96) && allDigits(p, len)) {
    words = (int)len / 4;
    for (int w = 0; w < words; w++) {
      int idx = (p[4 * w] - '0') * 1000 + (p[4 * w + 1] - '0') * 100 + (p[4 * w + 2] - '0') * 10 + (p[4 * w + 3] - '0');
      if (idx >= BIP39_WORDS) {
        mnemonic.clear();
        return 0;
      }
      const char *word = bip39Word(idx);
      if (!mnemonic.append(word, strlen(word)) || (w < words - 1 && !mnemonic.append(" ", 1))) {
        mnemonic.clear();
        return 0;
      }
    }
    *fmt = SEEDQR_STANDARD;
  } else if (len == 16 || len == 32) {
    const char *m = mnemonicFromEntropy(p, len);
    bool ok = m && mnemonic.append(m, strlen(m));
    mnemonic_clear(); // the static buffer mnemonicFromEntropy wrote into
    if (!ok) {
      mnemonic.clear();
      return 0;
    }
    words = len == 16 ? 12 : 24;
    *fmt = SEEDQR_COMPACT;
  } else {
    return 0;
  }
  if (!checkMnemonic(mnemonic.c_str())) { // standard can carry a bad checksum word
    mnemonic.clear();
    *fmt = SEEDQR_NONE;
    return 0;
  }
  return words;
}
