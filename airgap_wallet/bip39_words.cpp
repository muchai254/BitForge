#include "bip39_words.h"
#include <Bitcoin.h> // first: lets arduino-cli find uBitcoin for the trezor header below
#include <esp_random.h>
#include <string.h>
#include <utility/trezor/bip39.h>

const char *bip39Word(int i) {
  if (i < 0 || i >= BIP39_WORDS) return "";
  return mnemonic_wordlist()[i];
}

int bip39PrefixRange(const char *prefix, int *first) {
  const char *const *list = mnemonic_wordlist();
  size_t n = strlen(prefix);
  int start = -1, count = 0;
  for (int i = 0; i < BIP39_WORDS; i++) {
    if (strncmp(list[i], prefix, n) == 0) {
      if (start < 0) start = i;
      count++;
    } else if (start >= 0) {
      break; // sorted: the matches are contiguous
    }
  }
  if (first) *first = start;
  return count;
}

int bip39Index(const char *word) {
  int first;
  int n = bip39PrefixRange(word, &first);
  for (int i = first; n > 0 && i < first + n; i++) {
    if (strcmp(bip39Word(i), word) == 0) return i;
  }
  return -1;
}

int bip39RandomIndex() { return (int)(esp_random() % BIP39_WORDS); }
