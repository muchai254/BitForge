#include "qr_out.h"
#include <Arduino.h>
#include "display.h"
#include "secure_mem.h"
#include "src/qrgen/qrgen.h"

// Module bitmap for the largest version we allow ((97*97+7)/8 = 1177 bytes).
static uint8_t modules[(((QR_MAX_VERSION * 4 + 17) * (QR_MAX_VERSION * 4 + 17)) + 7) / 8];

QrInfo qrDraw(const uint8_t *data, size_t len, int x, int y, int w, int h, uint8_t ecc) {
  QrInfo info = {0, 0, 0};
  if (len > 0xFFFF) return info;
  QRCode qr;
  int version = 0;
  for (int v = 1; v <= QR_MAX_VERSION; v++) {
    if (qrcode_initBytes(&qr, modules, (uint8_t)v, ecc, (uint8_t *)data, (uint16_t)len) == 0) {
      version = v;
      break;
    }
  }
  if (!version) return info;

  int size = qr.size;
  int box = min(w, h);
  int px = box / (size + 2 * QR_QUIET);
  if (px < 1) {
    secure_zero(modules, sizeof modules);
    return info;
  }
  int total = px * (size + 2 * QR_QUIET);
  int ox = x + (w - total) / 2, oy = y + (h - total) / 2;
  tft.fillRect(ox, oy, total, total, COL_WHITE);
  int qx = ox + px * QR_QUIET, qy = oy + px * QR_QUIET;
  for (int my = 0; my < size; my++) {
    // Draw runs of dark modules as one rectangle (much faster than per module).
    int mx = 0;
    while (mx < size) {
      if (!qrcode_getModule(&qr, mx, my)) {
        mx++;
        continue;
      }
      int run = mx;
      while (run < size && qrcode_getModule(&qr, run, my)) run++;
      tft.fillRect(qx + mx * px, qy + my * px, (run - mx) * px, px, COL_BLACK);
      mx = run;
    }
  }
  secure_zero(modules, sizeof modules);
  secure_zero(&qr, sizeof qr);
  info.version = version;
  info.size = size;
  info.modulePx = px;
  return info;
}

static bool capacityCase(int version, uint8_t ecc, size_t capacity) {
  static uint8_t buf[300];
  memset(buf, 0xA5, sizeof buf); // not printable/numeric: byte mode
  QRCode qr;
  bool fits = qrcode_initBytes(&qr, modules, version, ecc, buf, capacity) == 0;
  bool refused = qrcode_initBytes(&qr, modules, version, ecc, buf, capacity + 1) != 0;
  secure_zero(modules, sizeof modules);
  Serial.printf("  v%d-%s %u bytes: %s, %u bytes: %s\n", version, ecc == QR_ECC_LOW ? "L" : "M",
                (unsigned)capacity, fits ? "encodes" : "REFUSED", (unsigned)(capacity + 1),
                refused ? "refused" : "ENCODED (overflow!)");
  return fits && refused;
}

bool qrSelfTest() {
  Serial.println("[qr] encoder capacity self-test (byte mode)");
  bool ok = true;
  ok &= capacityCase(1, QR_ECC_LOW, 17);
  ok &= capacityCase(1, QR_ECC_MEDIUM, 14);
  ok &= capacityCase(2, QR_ECC_LOW, 32);
  ok &= capacityCase(7, QR_ECC_LOW, 154);
  ok &= capacityCase(10, QR_ECC_LOW, 271);
  QRCode qr;
  bool badVersion = qrcode_initBytes(&qr, modules, 41, QR_ECC_LOW, (uint8_t *)"x", 1) != 0 &&
                    qrcode_initBytes(&qr, modules, 0, QR_ECC_LOW, (uint8_t *)"x", 1) != 0;
  Serial.printf("  version 0 and 41: %s\n", badVersion ? "refused" : "ACCEPTED");
  ok &= badVersion;
  Serial.printf("[qr] self-test %s\n", ok ? "PASS" : "FAIL");
  return ok;
}
