// Receives an unsigned PSBT from the camera, as Sparrow's animated
// UR QR (ur:crypto-psbt, or ur:psbt) or a single QR holding base64
// ("cHNidP8..."), with a progress bar while the fragments arrive.
// Only the transport is checked here (UR checksums, PSBT magic, size); the
// PSBT itself is parsed and verified afterwards.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PSBT_MAX_BYTES 65536 // prototype size limit

// Runs until a PSBT is complete or the user cancels. On success *psbt is a
// malloc'ed buffer the caller owns (free it) and true is returned.
bool psbtScanScreen(uint8_t **psbt, size_t *len);
