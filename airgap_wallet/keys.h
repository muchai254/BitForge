// Key handling: BIP39 mnemonic -> BIP32 root -> BIP84 account on
// signet. Nothing here is ever written to flash.
#pragma once
#include <Arduino.h>

// Checks the public BIP39 test mnemonic against Sparrow's values (fingerprint,
// tpub, first receive/change address). Printing them to Serial is allowed only
// because this mnemonic is public. Returns true if all match.
bool keysRunTestVector();

// Master fingerprint of a BIP39 mnemonic + passphrase (signet). The caller
// must check the mnemonic first (checkMnemonic) and call
// secure_scrub_stack() once its own secret work is done: the uBitcoin key
// objects created in here leave nothing in RAM after that.
void keysFingerprint(const char *mnemonic, size_t mnemonicLen, const char *passphrase, size_t passLen,
                     uint8_t fp[4], void (*progress)(float));

// Public data of the BIP84 signet account m/84'/1'/0' (nothing secret).
#define KEYS_ADDRESSES 5
struct AccountInfo {
  uint8_t fingerprint[4];
  char tpub[120];                    // plain BIP32 version bytes (tpub), as Sparrow shows
  char receive[KEYS_ADDRESSES][80];  // m/84'/1'/0'/0/0..4
  char change[2][80];                // m/84'/1'/0'/1/0..1
};

// Derives the account from mnemonic + passphrase. Same rule as
// keysFingerprint(): check the mnemonic first, scrub the stack afterwards.
void keysAccount(const char *mnemonic, size_t mnemonicLen, const char *passphrase, size_t passLen,
                 AccountInfo &out, void (*progress)(float));
