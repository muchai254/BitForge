// Access to the BIP39 English word list (from uBitcoin's trezor-crypto copy).
#pragma once
#include <stddef.h>

#define BIP39_WORDS 2048

const char *bip39Word(int i);

// Index of the first word starting with prefix, and how many words do
// (the list is sorted). Returns 0 matches if none.
int bip39PrefixRange(const char *prefix, int *first);

// Index of word, or -1.
int bip39Index(const char *word);

// A uniformly random word index (for quiz distractors; not secret).
int bip39RandomIndex();
