// SeedQR (SeedSigner's spec): a seed as a QR code.
//   Standard SeedQR: each word's BIP39 index as 4 decimal digits, joined
//                    (12 words -> 48 digits, 24 -> 96). Numeric mode.
//   CompactSeedQR:   the raw BIP39 entropy, without the checksum bits
//                    (12 words -> 16 bytes, 24 -> 32). Byte mode.
// Everything here is secret; the caller wipes buffers and scrubs the stack.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "secure_mem.h"
#include "session.h"

enum SeedQrFormat { SEEDQR_NONE, SEEDQR_STANDARD, SEEDQR_COMPACT };

// True if a payload has the shape of a SeedQR (used to keep such payloads out
// of logs and test screens even when they aren't loaded as a seed).
bool seedqrLooksLike(const uint8_t *p, size_t len);

// Encodes the mnemonic. out receives 48/96 digits (standard) or 16/32 bytes
// (compact). Returns the length, or 0 on error.
size_t seedqrEncode(const char *mnemonic, SeedQrFormat fmt, SecretBuffer<97> &out);

// Decodes a scanned payload into a space-separated mnemonic (checksum
// verified). Returns the number of words (12/24), or 0 if the payload isn't a
// valid SeedQR. *fmt is set to the format found.
int seedqrDecode(const uint8_t *p, size_t len, SecretBuffer<SESSION_MNEMONIC_CAP> &mnemonic, SeedQrFormat *fmt);
