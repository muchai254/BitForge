// CAM -> wallet frame format. Spec: docs/protocol.md section 1.
//   #QR,<len>,<crc32>,<payload>\n   payload is printable ASCII (0x20..0x7E)
//   #QB,<len>,<crc32>,<base64>\n    anything else
// <len> is the decimal length of the RAW payload, <crc32> is 8 lowercase hex
// digits of CRC-32/ISO-HDLC (zlib) over the RAW payload.
#pragma once
#include <Arduino.h>

#define LINK_MAX_PAYLOAD 2953 // largest QR code (version 40-L, byte mode)

uint32_t crc32Ieee(const uint8_t *data, size_t len);

// Writes one frame for the payload to the stream. Returns false (writing
// nothing) if the payload is empty or larger than LINK_MAX_PAYLOAD.
bool writeLinkFrame(Print &out, const uint8_t *payload, size_t len);
