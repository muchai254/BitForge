// The current signing session: the seed lives here, in RAM only, until the
// session is wiped. Nothing in here is ever written to flash or printed.
#pragma once
#include <stdint.h>
#include "secure_mem.h"

#define SESSION_MNEMONIC_CAP 256 // 24 words * (8 letters + space) = 216
#define SESSION_PASSPHRASE_CAP 65 // up to 64 characters

struct Session {
  bool loaded;
  SecretBuffer<SESSION_MNEMONIC_CAP> mnemonic;     // space-separated BIP39 words
  SecretBuffer<SESSION_PASSPHRASE_CAP> passphrase; // BIP39 passphrase (may be empty)
  uint8_t fingerprint[4];                          // master fingerprint (public)
  uint8_t words;                                   // 12/24
  uint16_t diceRolls;                              // how the seed was made (for the summary)
  bool generatedHere;
  bool backupConfirmed;                            // passed the backup quiz
};

extern Session session;

void sessionWipe();

// Copies word n (0-based) of the session mnemonic into out (cap >= 9).
// Returns false if there's no such word.
bool sessionWord(int n, char *out, size_t cap);
