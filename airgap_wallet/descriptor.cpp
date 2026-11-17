#include "descriptor.h"
#include <stdio.h>
#include <string.h>

// Port of Bitcoin Core's descriptor checksum (descriptor.cpp). Checked on the
// PC against "raw(deadbeef)#89f8spxm" (BIP380) and Bitcoin Core's
// "pkh([d34db33f/44'/0'/0']xpub6ERA.../1/*)#ml40v0wf" before porting.
static const char INPUT_CHARSET[] =
    "0123456789()[],'/*abcdefgh@:$%{}"
    "IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
    "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
static const char CHECKSUM_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static uint64_t polyMod(uint64_t c, int val) {
  uint8_t c0 = c >> 35;
  c = ((c & 0x7ffffffffULL) << 5) ^ (uint64_t)val;
  if (c0 & 1) c ^= 0xf5dee51989ULL;
  if (c0 & 2) c ^= 0xa9fdca3312ULL;
  if (c0 & 4) c ^= 0x1bab10e32dULL;
  if (c0 & 8) c ^= 0x3706b1677aULL;
  if (c0 & 16) c ^= 0x644d626ffdULL;
  return c;
}

bool descriptorChecksum(const char *desc, char out[9]) {
  uint64_t c = 1;
  int cls = 0, clscount = 0;
  for (const char *p = desc; *p; p++) {
    const char *f = strchr(INPUT_CHARSET, *p);
    if (!f) return false;
    int pos = (int)(f - INPUT_CHARSET);
    c = polyMod(c, pos & 31);
    cls = cls * 3 + (pos >> 5);
    if (++clscount == 3) {
      c = polyMod(c, cls);
      cls = 0;
      clscount = 0;
    }
  }
  if (clscount > 0) c = polyMod(c, cls);
  for (int j = 0; j < 8; j++) c = polyMod(c, 0);
  c ^= 1;
  for (int j = 0; j < 8; j++) out[j] = CHECKSUM_CHARSET[(c >> (5 * (7 - j))) & 31];
  out[8] = 0;
  return true;
}

bool keyWithOrigin(const uint8_t fp[4], const char *tpub, char *out, size_t cap) {
  int n = snprintf(out, cap, "[%02x%02x%02x%02x/84h/1h/0h]%s", fp[0], fp[1], fp[2], fp[3], tpub);
  return n > 0 && (size_t)n < cap;
}

bool descriptorWpkhAccount(const uint8_t fp[4], const char *tpub, char *out, size_t cap) {
  char key[160];
  if (!keyWithOrigin(fp, tpub, key, sizeof key)) return false;
  int n = snprintf(out, cap, "wpkh(%s/<0;1>/*)", key);
  if (n <= 0 || (size_t)n + 9 >= cap) return false;
  char cs[9];
  if (!descriptorChecksum(out, cs)) return false;
  out[n] = '#';
  memcpy(out + n + 1, cs, 9);
  return true;
}
