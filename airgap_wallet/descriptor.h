// Output descriptors for the watch-only export (BIP380/BIP384/BIP389).
#pragma once
#include <stddef.h>
#include <stdint.h>

// BIP380 descriptor checksum of desc (8 characters + NUL into out[9]).
// Returns false if desc has a character outside the descriptor charset.
bool descriptorChecksum(const char *desc, char out[9]);

// "wpkh([<fp>/84h/1h/0h]<tpub>/<0;1>/*)#<checksum>" into out. Returns false
// if it doesn't fit.
bool descriptorWpkhAccount(const uint8_t fp[4], const char *tpub, char *out, size_t cap);

// "[<fp>/84h/1h/0h]<tpub>" (key with origin) into out.
bool keyWithOrigin(const uint8_t fp[4], const char *tpub, char *out, size_t cap);
