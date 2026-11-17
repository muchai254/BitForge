#include "camlink.h"
#include "config.h"
#include "secure_mem.h"
#include <string.h>

#define CAMLINK_BAUD 115200
// At 115200 the link delivers ~11.5 KB/s; 16 KB covers >1 s of the UI not
// reading (e.g. while the scan screen redraws).
#define CAMLINK_UART_RX_BUFFER 16384

// "#QB,2953,xxxxxxxx," is 18 bytes; base64 of the largest payload is 3940.
#define LINE_CAP (18 + 4 * ((CAMLINK_MAX_PAYLOAD + 2) / 3))

// Large buffers are static (not on the 8 KB loop stack).
static uint8_t line[LINE_CAP]; // the frame being received, from its '#'
static size_t lineLen = 0;
static uint8_t scratch[CAMLINK_MAX_PAYLOAD]; // frame being checked
static uint8_t payload[CAMLINK_MAX_PAYLOAD]; // last valid payload, kept until the next one
static size_t payloadLen = 0;
static uint32_t payloadCrc = 0;
static bool payloadBase64 = false;
static CamLinkStats stats = {};

enum Verdict { V_OK, V_BAD_HEADER, V_CRC, V_OVERSIZE, V_MALFORMED };

// CRC-32/ISO-HDLC (docs/protocol.md; same as airgap_qrcam/link_frame.cpp).
static uint32_t crc32Ieee(const uint8_t *p, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static int base64Value(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Strict base64 decode of in[0..n) into out (which has room for `expected`
// bytes). Fails unless the input decodes to exactly `expected` bytes.
static bool base64Decode(const uint8_t *in, size_t n, uint8_t *out, size_t expected) {
  if (n == 0 || n % 4 != 0) return false;
  size_t pad = in[n - 1] == '=' ? (in[n - 2] == '=' ? 2 : 1) : 0;
  if (n / 4 * 3 - pad != expected) return false;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    bool last = i + 4 == n;
    int v[4];
    for (int k = 0; k < 4; k++) {
      if (last && k >= 4 - (int)pad) {
        if (in[i + k] != '=') return false;
        v[k] = 0;
      } else {
        v[k] = base64Value(in[i + k]);
        if (v[k] < 0) return false; // also rejects '=' anywhere but the end
      }
    }
    uint32_t q = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    out[o++] = (uint8_t)(q >> 16);
    if (o < expected) out[o++] = (uint8_t)(q >> 8);
    if (o < expected) out[o++] = (uint8_t)q;
  }
  return o == expected;
}

static int hexValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1; // uppercase is not allowed by the protocol
}

// Validates line[0..lineLen) (starting with '#', without its '\n') and, if it's
// a valid frame, stores its payload.
static Verdict checkLine() {
  const uint8_t *p = line;
  size_t n = lineLen;
  // "#QR," or "#QB,"
  if (n < 4 || p[0] != '#' || p[1] != 'Q' || (p[2] != 'R' && p[2] != 'B') || p[3] != ',') return V_BAD_HEADER;
  bool b64 = p[2] == 'B';
  size_t i = 4;
  // <len>: 1..4 digits, no leading zero
  size_t len = 0, digits = 0;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    if (digits == 0 && p[i] == '0') return V_MALFORMED;
    if (++digits > 4) return V_MALFORMED;
    len = len * 10 + (p[i] - '0');
    i++;
  }
  if (digits == 0 || i >= n || p[i] != ',') return V_MALFORMED;
  if (len > CAMLINK_MAX_PAYLOAD) return V_OVERSIZE;
  i++;
  // <crc>: exactly 8 lowercase hex digits
  uint32_t crc = 0;
  for (int k = 0; k < 8; k++, i++) {
    int v = i < n ? hexValue(p[i]) : -1;
    if (v < 0) return V_MALFORMED;
    crc = (crc << 4) | (uint32_t)v;
  }
  if (i >= n || p[i] != ',') return V_MALFORMED;
  i++;
  const uint8_t *body = p + i;
  size_t bodyLen = n - i;

  if (b64) {
    if (!base64Decode(body, bodyLen, scratch, len)) return V_MALFORMED;
  } else {
    if (bodyLen != len) return V_MALFORMED;
    for (size_t k = 0; k < bodyLen; k++) {
      if (body[k] < 0x20 || body[k] > 0x7E) return V_MALFORMED;
    }
    memcpy(scratch, body, len);
  }
  if (crc32Ieee(scratch, len) != crc) return V_CRC;
  memcpy(payload, scratch, len);
  payloadLen = len;
  payloadCrc = crc;
  payloadBase64 = b64;
  return V_OK;
}

// Drops the current '#' and moves to the next '#' already in the buffer (the
// next frame may start inside a truncated one), or empties the buffer.
static void resyncInLine() {
  size_t next = 1;
  while (next < lineLen && line[next] != '#') next++;
  stats.skipped += next;
  lineLen -= next;
  memmove(line, line + next, lineLen);
}

static void countReject(Verdict v) {
  if (v == V_CRC) stats.crcFail++;
  else if (v == V_OVERSIZE) stats.oversize++;
  else if (v == V_MALFORMED) stats.malformed++;
  // V_BAD_HEADER: a '#' that doesn't start a frame; counted in `skipped`.
}

// Called at '\n'. Tries the buffered line, then any '#' inside it.
static bool lineComplete() {
  while (lineLen > 0) {
    Verdict v = checkLine();
    if (v == V_OK) {
      lineLen = 0;
      stats.ok++;
      return true;
    }
    countReject(v);
    resyncInLine();
  }
  return false;
}

static bool feed(uint8_t b) {
  if (lineLen == 0) {
    if (b == '#') line[lineLen++] = b;
    else stats.skipped++;
    return false;
  }
  if (b == '\n') return lineComplete();
  if (lineLen == LINE_CAP) {
    stats.oversize++;
    resyncInLine();
    if (lineLen == 0) { // no '#' left: this byte may start the next frame
      if (b == '#') line[lineLen++] = b;
      else stats.skipped++;
      return false;
    }
  }
  line[lineLen++] = b;
  return false;
}

static void onRxError(hardwareSerial_error_t err) {
  if (err == UART_BUFFER_FULL_ERROR) stats.rxOverflow++;     // our ring buffer was full
  else if (err == UART_FIFO_OVF_ERROR) stats.rxFifoOverflow++; // the hardware FIFO wasn't drained in time
}

void camlinkBegin() {
  Serial2.setRxBufferSize(CAMLINK_UART_RX_BUFFER); // must come before begin()
  // RX only: no TX pin is assigned, so this board can never send to the camera.
  Serial2.begin(CAMLINK_BAUD, SERIAL_8N1, PIN_CAM_RX, -1);
  Serial2.onReceiveError(onRxError);
}

void camlinkReset() {
  while (Serial2.available()) Serial2.read();
  lineLen = 0;
  payloadLen = 0;
  stats = {};
}

void camlinkZeroBuffers() {
  secure_zero(line, sizeof line);
  secure_zero(scratch, sizeof scratch);
  secure_zero(payload, sizeof payload);
  lineLen = 0;
  payloadLen = 0;
  payloadCrc = 0;
}

size_t camlinkWipe() {
  while (Serial2.available()) Serial2.read();
  secure_zero(line, sizeof line);
  secure_zero(scratch, sizeof scratch);
  secure_zero(payload, sizeof payload);
  lineLen = 0;
  payloadLen = 0;
  payloadCrc = 0;
  Serial2.end(); // frees the driver's RX ring buffer (not zeroed by the driver)
  size_t zeroed = secure_scrub_free_heap();
  camlinkBegin();
  stats = {};
  return zeroed;
}

bool camlinkPoll() {
  while (Serial2.available()) {
    if (feed((uint8_t)Serial2.read())) return true; // leave the rest for the next call
  }
  return false;
}

const uint8_t *camlinkPayload() { return payload; }
size_t camlinkPayloadLen() { return payloadLen; }
uint32_t camlinkPayloadCrc() { return payloadCrc; }
bool camlinkPayloadWasBase64() { return payloadBase64; }
const CamLinkStats &camlinkStats() { return stats; }
