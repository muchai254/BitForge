// One-way camera link: receives framed QR payloads from the ESP32-CAM on
// Serial2 RX (GPIO34). Implements the receiver rules of docs/protocol.md §1.
// Every byte from the camera is treated as hostile: lengths are bounded, the
// CRC is checked, and nothing is delivered unless the whole frame is valid.
// This module never prints payloads (they will include seed QR codes).
#pragma once
#include <Arduino.h>

#define CAMLINK_MAX_PAYLOAD 2953 // largest QR code (version 40-L, byte mode)

struct CamLinkStats {
  uint32_t ok;          // valid frames delivered
  uint32_t crcFail;     // well-formed frame, wrong CRC
  uint32_t oversize;    // len > max, or a line longer than the buffer
  uint32_t malformed;   // anything else wrong with a frame
  uint32_t skipped;     // bytes discarded while looking for a frame
  uint32_t rxOverflow;     // UART receive ring buffer full (bytes lost)
  uint32_t rxFifoOverflow; // UART hardware FIFO overflow (bytes lost)
};

void camlinkBegin();

// Discards everything received so far (the UART keeps receiving while other
// screens run) and zeroes the counters.
void camlinkReset();

// After a secret came over the link (a SeedQR): zeroes every buffer that
// held it here, restarts the UART so its driver frees the receive ring buffer,
// then zeroes the free heap (which now includes that buffer). Returns the
// number of free-heap bytes zeroed.
size_t camlinkWipe();

// Zeroes this module's buffers only (no UART calls). For the lock task,
// which must not touch Serial2 while the UI task is stopped.
void camlinkZeroBuffers();

// Processes received bytes. Returns true when a new valid payload is ready;
// it stays available through camlinkPayload() until the next valid payload
// (rejected frames never overwrite it).
bool camlinkPoll();

const uint8_t *camlinkPayload();
size_t camlinkPayloadLen();
uint32_t camlinkPayloadCrc();
bool camlinkPayloadWasBase64(); // sent as #QB (not all printable)

const CamLinkStats &camlinkStats();
