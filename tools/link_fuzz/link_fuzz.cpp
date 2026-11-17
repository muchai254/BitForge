#include "link_fuzz.h"
#include "camera_pins.h"
#include "link_frame.h"

#define FUZZ_GAP_MS 100       // pause between items, so the wallet can redraw
#define FUZZ_REPORT_MS 10000

// Collects a frame in memory so it can be corrupted before sending.
class BufPrint : public Print {
 public:
  uint8_t buf[4200];
  size_t len = 0;
  size_t write(uint8_t c) override {
    if (len < sizeof buf) buf[len++] = c;
    return 1;
  }
  void clear() { len = 0; }
};

static BufPrint fb;
static uint8_t payload[400];
static uint32_t rng = 0x12345678;
static uint32_t items = 0, validSent = 0, badSent = 0;
static uint32_t lastReport = 0;

static uint32_t rnd() { // xorshift32
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
static uint32_t rndRange(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

static size_t makePayload(bool printable) {
  size_t n = rndRange(1, sizeof payload);
  for (size_t i = 0; i < n; i++) payload[i] = printable ? (uint8_t)rndRange(0x20, 0x7E) : (uint8_t)rnd();
  if (!printable) payload[0] = 0x00; // make sure it goes out as #QB
  return n;
}

static void buildValid(bool printable) {
  fb.clear();
  writeLinkFrame(fb, payload, makePayload(printable));
}

static void sendBuf() { Serial1.write(fb.buf, fb.len); }

static void sendValid(bool printable) {
  buildValid(printable);
  sendBuf();
  validSent++;
}

static void sendString(const char *s) {
  Serial1.write((const uint8_t *)s, strlen(s));
  badSent++;
}

void fuzzBegin() {
  rng ^= esp_random() | 1;
  Serial.println("LINK FUZZ BUILD: sending fuzz on the wallet link (no QR scanning)");
}

void fuzzStep() {
  items++;
  switch (rnd() % 10) {
    case 0:
    case 1:
      sendValid(true);
      break;
    case 2:
      sendValid(false);
      break;
    case 3: { // raw garbage, any byte values
      size_t n = rndRange(1, 200);
      for (size_t i = 0; i < n; i++) Serial1.write((uint8_t)rnd());
      badSent++;
      break;
    }
    case 4: { // one bit flipped (CRC-32 catches every single-bit error)
      buildValid(rnd() & 1);
      fb.buf[rnd() % fb.len] ^= (uint8_t)(1u << (rnd() % 8));
      sendBuf();
      badSent++;
      break;
    }
    case 5: { // truncated frame, then straight into a valid one
      buildValid(true);
      Serial1.write(fb.buf, rndRange(1, fb.len - 1));
      badSent++;
      sendValid(rnd() & 1);
      break;
    }
    case 6: { // wrong CRC: change one hex digit of the crc field
      buildValid(true);
      uint8_t *p = fb.buf;
      int commas = 0;
      size_t i = 0;
      while (i < fb.len && commas < 2) if (p[i++] == ',') commas++;
      p[i] = p[i] == '0' ? '1' : '0';
      sendBuf();
      badSent++;
      break;
    }
    case 7: // oversize: len too big, or a line longer than the wallet's buffer
      if (rnd() & 1) {
        sendString("#QR,5000,00000000,too long\n");
      } else {
        sendString("#QR,2953,00000000,");
        for (int i = 0; i < 5000; i++) Serial1.write('A');
        Serial1.write('\n');
      }
      break;
    case 8: { // format errors
      static const char *bad[] = {
          "#QR,05,3610a686,hello\n",   // leading zero
          "#QR,5,3610A686,hello\n",    // uppercase crc
          "#QB,3,00000000,AA*A\n",     // bad base64
          "#QZ,5,3610a686,hello\n",    // unknown type
          "#QR,,3610a686,hello\n",     // empty len
          "#QR,5,3610a68,hello\n",     // 7-digit crc
          "#QR,5,3610a686hello\n",     // missing comma
          "#QR,5,3610a686,hel\x01o\n", // non-printable in #QR
          "#\n",
          "##########\n",
      };
      sendString(bad[rnd() % (sizeof bad / sizeof bad[0])]);
      break;
    }
    default: { // valid frame whose payload contains '#' and ','
      size_t n = rndRange(8, 120);
      for (size_t i = 0; i < n; i++) payload[i] = (i % 3 == 0) ? '#' : (i % 3 == 1) ? ',' : (uint8_t)rndRange(0x30, 0x7A);
      fb.clear();
      writeLinkFrame(fb, payload, n);
      sendBuf();
      validSent++;
      break;
    }
  }
  delay(FUZZ_GAP_MS);

  uint32_t now = millis();
  if (now - lastReport >= FUZZ_REPORT_MS) {
    Serial.printf("[fuzz] items %lu, valid frames sent %lu, bad items sent %lu, up %lus\n",
                  (unsigned long)items, (unsigned long)validSent, (unsigned long)badSent,
                  (unsigned long)(now / 1000));
    lastReport = now;
  }
}
