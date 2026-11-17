#include "session.h"
#include <string.h>

Session session;

void sessionWipe() {
  session.mnemonic.clear();
  session.passphrase.clear();
  secure_zero(session.fingerprint, sizeof session.fingerprint);
  session.words = 0;
  session.diceRolls = 0;
  session.generatedHere = false;
  session.backupConfirmed = false;
  session.loaded = false;
}

bool sessionWord(int n, char *out, size_t cap) {
  const char *p = session.mnemonic.c_str();
  for (int w = 0; w < n; w++) {
    p = strchr(p, ' ');
    if (!p) return false;
    p++;
  }
  size_t len = strcspn(p, " ");
  if (len == 0 || len >= cap) return false;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}
