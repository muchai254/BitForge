#include "link_frame.h"

uint32_t crc32Ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static bool isPrintable(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] < 0x20 || p[i] > 0x7E) return false;
  }
  return true;
}

static void writeBase64(Print &out, const uint8_t *p, size_t len) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char quad[4];
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16;
    if (i + 1 < len) v |= (uint32_t)p[i + 1] << 8;
    if (i + 2 < len) v |= p[i + 2];
    quad[0] = tbl[(v >> 18) & 63];
    quad[1] = tbl[(v >> 12) & 63];
    quad[2] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
    quad[3] = i + 2 < len ? tbl[v & 63] : '=';
    out.write((const uint8_t *)quad, 4);
  }
}

bool writeLinkFrame(Print &out, const uint8_t *payload, size_t len) {
  if (len == 0 || len > LINK_MAX_PAYLOAD) return false;
  bool text = isPrintable(payload, len);
  char head[32];
  int n = snprintf(head, sizeof head, "#%s,%u,%08lx,", text ? "QR" : "QB",
                   (unsigned)len, (unsigned long)crc32Ieee(payload, len));
  out.write((const uint8_t *)head, n);
  if (text) out.write(payload, len);
  else writeBase64(out, payload, len);
  out.write('\n');
  return true;
}
