// BitForge: ESP32-CAM QR scanner firmware.
//
// Grabs grayscale frames, decodes QR codes with quirc, and sends each decoded
// payload to the wallet as a framed line on GPIO14 (TX only; see
// docs/protocol.md). This device is UNTRUSTED from the wallet's point of view:
// it never receives anything from the wallet, and holds no secrets.
//
// UART0 (the USB-TTL adapter) at 2,000,000 baud is a development console only:
//   - stats every 5 s; "QR <len> bytes crc <crc>: <text>" for every decode
//   - single-letter commands: l = toggle flash LED, p = preview on,
//     o = preview off; image tuning (lower/upper case = down/up):
//     e/E ae level, c/C contrast, b/B brightness, x auto/manual exposure,
//     a/A manual exposure value, r reset. Each prints "[tune] ...".
//   - preview: "@FRM4 <w> <h> <len> <codes>\n", then <len> = w*h/2 bytes of
//     4-bit grayscale (two pixels per byte, high nibble first), then "\n"
//     (160x120, up to 10 times a second) for tools/cam_viewer.
//     <codes> = QR codes quirc located in that frame (decoded or not).
//     The trailing "\n" makes every text line after a frame start a fresh
//     line, so a reader that loses bytes resyncs at the next line.
//     The preview goes through a TX ring buffer and is skipped while the
//     previous one is still being sent, so it never stalls decoding.
// The red LED on the back blinks whenever a new QR payload is decoded.
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include "camera_pins.h"
#include "link_frame.h"
extern "C" {
#include "src/quirc/quirc.h"
}

#define FW_VERSION "0.3.9-phase3"

// quirc_decode() keeps a ~8.9 KB struct datastream on the stack, which
// overflows the default 8 KB loop task stack as soon as a QR code is in view.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);
#define CONSOLE_BAUD 2000000      // exact divisor on both the ESP32 and the FT232R adapter
#define CONSOLE_TX_BUFFER 16384   // holds a whole preview frame plus debug lines
#define DUPLICATE_HOLDOFF_MS 1500 // don't resend the same payload within this time
#define PREVIEW_W 160
#define PREVIEW_H 120
#define PREVIEW_BYTES (PREVIEW_W * PREVIEW_H / 2) // 4 bits per pixel
#define PREVIEW_INTERVAL_MS 100
#define LED_BLINK_MS 300
#define CAMERA_INIT_ATTEMPTS 3
#define QUIRC_INTERNAL_LIMIT (128 * 1024) // quirc allocations below this prefer internal RAM
#define MAX_GRAB_FAILURES 20        // consecutive failed grabs (~2 s) before a restart

// One fixed resolution. VGA decoded at only 2-3 fps, and re-initialising the
// camera to switch sizes occasionally left it dead (the sensor failed its ID
// probe). In GRAYSCALE the driver can't change size without a re-init
// (set_framesize() makes it drop every frame: "cam_hal: FB-SIZE").
#define FRAME_SIZE FRAMESIZE_QVGA

static struct quirc *qr = nullptr;
static bool flashOn = false;

static bool previewOn = false;
static uint32_t lastPreviewMs = 0;
static uint8_t preview[PREVIEW_BYTES];

static uint32_t redLedOffAt = 0;
static int grabFailures = 0;

// quirc structures are large (~4 KB and ~9 KB); keep them off the stack.
static struct quirc_code code;
static struct quirc_data data;

static uint32_t statFrames = 0, statDecoded = 0, statSent = 0, statDecodeErrors = 0;
static uint32_t statSince = 0;
// Time per stage, summed over the stats interval (microseconds).
static uint64_t usGrab = 0, usEnd = 0, usDecode = 0;

// Duplicate suppression.
static uint32_t lastCrc = 0;
static size_t lastLen = 0;
static uint32_t lastSentMs = 0;

// The sensor's ID probe occasionally fails ("Detected camera not supported"),
// so init is retried with a sensor power-cycle in between.
static bool cameraInitOnce(framesize_t size) {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_GRAYSCALE;
  c.frame_size = size;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// Holds the sensor in power-down for a moment so a failed probe can be retried
// from a clean state.
static void powerCycleSensor() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(200);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(200);
}

static bool cameraInit(framesize_t size) {
  for (int attempt = 1; attempt <= CAMERA_INIT_ATTEMPTS; attempt++) {
    if (cameraInitOnce(size)) return true;
    Serial.printf("camera init attempt %d of %d failed\n", attempt, CAMERA_INIT_ATTEMPTS);
    powerCycleSensor();
  }
  return false;
}

// Sizes the quirc buffer for frames of `size`. quirc thresholds and
// flood-fills its image in place with scattered accesses, so its allocations
// are steered to internal RAM (QVGA: 76.8 KB) instead of PSRAM. malloc falls
// back to PSRAM if they don't fit.
static bool resizeQuirc(framesize_t size) {
  int w = resolution[size].width, h = resolution[size].height;
  heap_caps_malloc_extmem_enable(QUIRC_INTERNAL_LIMIT);
  int r = quirc_resize(qr, w, h);
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  if (r < 0) {
    Serial.println("quirc_resize failed (out of memory?)");
    return false;
  }
  const uint8_t *img = quirc_begin(qr, nullptr, nullptr);
  Serial.printf("frame size %dx%d, quirc buffer in %s, free PSRAM %u, free internal %u (largest block %u)\n",
                w, h, esp_ptr_external_ram(img) ? "PSRAM" : "internal RAM",
                (unsigned)ESP.getFreePsram(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

static void setFlash(bool on) {
  flashOn = on;
  digitalWrite(FLASH_LED_GPIO, on ? HIGH : LOW);
}

// Image tuning, adjustable live from the dev console (screens are much
// brighter than paper; the defaults are for a room). Ranges per esp32-camera:
// ae level / contrast / brightness -2..2; manual exposure value 0..1200.
struct Tuning {
  int aeLevel, contrast, brightness;
  bool autoExposure;
  int aecValue;
};
static Tuning tuning = {0, 0, 0, true, 300};

static void applyTuning() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_ae_level(s, tuning.aeLevel);
  s->set_contrast(s, tuning.contrast);
  s->set_brightness(s, tuning.brightness);
  s->set_exposure_ctrl(s, tuning.autoExposure ? 1 : 0);
  if (!tuning.autoExposure) s->set_aec_value(s, tuning.aecValue);
}

static void printTuning() {
  Serial.printf("[tune] ae level %d, contrast %d, brightness %d, exposure %s%s\n", tuning.aeLevel,
                tuning.contrast, tuning.brightness, tuning.autoExposure ? "auto" : "manual ",
                tuning.autoExposure ? "" : String(tuning.aecValue).c_str());
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void handleConsole() {
  while (Serial.available()) {
    char ch = Serial.read();
    bool tuned = true;
    switch (ch) {
      case 'l': setFlash(!flashOn); Serial.printf("flash LED %s\n", flashOn ? "on" : "off"); tuned = false; break;
      case 'p': previewOn = true; tuned = false; break;
      case 'o': previewOn = false; tuned = false; break;
      case 'e': tuning.aeLevel = clampInt(tuning.aeLevel - 1, -2, 2); break;
      case 'E': tuning.aeLevel = clampInt(tuning.aeLevel + 1, -2, 2); break;
      case 'c': tuning.contrast = clampInt(tuning.contrast - 1, -2, 2); break;
      case 'C': tuning.contrast = clampInt(tuning.contrast + 1, -2, 2); break;
      case 'b': tuning.brightness = clampInt(tuning.brightness - 1, -2, 2); break;
      case 'B': tuning.brightness = clampInt(tuning.brightness + 1, -2, 2); break;
      case 'x': tuning.autoExposure = !tuning.autoExposure; break;
      case 'a': tuning.aecValue = clampInt(tuning.aecValue - 50, 0, 1200); break;
      case 'A': tuning.aecValue = clampInt(tuning.aecValue + 50, 0, 1200); break;
      case 'r': tuning = {0, 0, 0, true, 300}; break;
      default: tuned = false;
    }
    if (tuned) {
      applyTuning();
      printTuning();
    }
  }
}

// Nearest-neighbour downscale of a grayscale frame into the preview buffer,
// packed to 4 bits per pixel (high nibble = left pixel).
static void makePreview(const uint8_t *img, int w, int h) {
  uint8_t *out = preview;
  for (int y = 0; y < PREVIEW_H; y++) {
    const uint8_t *row = img + (size_t)(y * h / PREVIEW_H) * w;
    for (int x = 0; x < PREVIEW_W; x += 2) {
      *out++ = (row[x * w / PREVIEW_W] & 0xF0) | (row[(x + 1) * w / PREVIEW_W] >> 4);
    }
  }
}

// True if the console TX buffer can take a whole preview frame without
// blocking, with some room left for debug lines.
static bool previewFits() {
  return Serial.availableForWrite() >= PREVIEW_BYTES + 256;
}

static void sendPreview(int codes) {
  Serial.printf("@FRM4 %d %d %d %d\n", PREVIEW_W, PREVIEW_H, PREVIEW_BYTES, codes);
  Serial.write(preview, sizeof preview);
  Serial.write('\n'); // ends the frame; see the header comment
}

// Same test as the wallet's seedqrLooksLike(): Standard SeedQR = 48/96 digits,
// CompactSeedQR = 16/32 bytes.
static bool looksLikeSeedQr(const uint8_t *p, size_t len) {
  if (len == 16 || len == 32) return true;
  if (len != 48 && len != 96) return false;
  for (size_t i = 0; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
  }
  return true;
}

static void sendPayload(const uint8_t *p, size_t len) {
  uint32_t crc = crc32Ieee(p, len);
  uint32_t now = millis();
  if (crc == lastCrc && len == lastLen && now - lastSentMs < DUPLICATE_HOLDOFF_MS) {
    lastSentMs = now; // still in view: keep suppressing
    return;
  }
  if (!writeLinkFrame(Serial1, p, len)) return;
  lastCrc = crc;
  lastLen = len;
  lastSentMs = now;
  statSent++;

  digitalWrite(RED_LED_GPIO, LOW); // on (active low)
  redLedOffAt = now + LED_BLINK_MS;

  // Debug echo of the whole payload (non-printable bytes shown as '.'), except
  // for anything shaped like a SeedQR: neither its content nor its CRC (32
  // bits of a seed) goes to the console.
  if (looksLikeSeedQr(p, len)) {
    Serial.printf("QR %u bytes: looks like a SeedQR (not shown)\n", (unsigned)len);
    return;
  }
  Serial.printf("QR %u bytes crc %08lx: ", (unsigned)len, (unsigned long)crc);
  for (size_t i = 0; i < len; i++) Serial.write(p[i] >= 0x20 && p[i] <= 0x7E ? p[i] : '.');
  Serial.println();
}

void setup() {
  Serial.setTxBufferSize(CONSOLE_TX_BUFFER); // must come before begin()
  Serial.begin(CONSOLE_BAUD);
  delay(200);
  Serial.printf("\nairgap_qrcam %s\n", FW_VERSION);

  pinMode(FLASH_LED_GPIO, OUTPUT);
  setFlash(false);
  pinMode(RED_LED_GPIO, OUTPUT);
  digitalWrite(RED_LED_GPIO, HIGH); // off (active low)

  // TX-only link to the wallet: no RX pin is assigned.
  Serial1.begin(LINK_BAUD, SERIAL_8N1, -1, LINK_TX_GPIO);

  if (!psramFound()) Serial.println("WARNING: no PSRAM found");
  qr = quirc_new();
  if (!qr || !cameraInit(FRAME_SIZE) || !resizeQuirc(FRAME_SIZE)) {
    Serial.println("FATAL: init failed, restarting in 5 s");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("quirc %s ready; link TX on GPIO%d at %d baud\n", quirc_version(), LINK_TX_GPIO, LINK_BAUD);
  statSince = millis();
}

void loop() {
  handleConsole();

  int64_t tStart = esp_timer_get_time();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("frame grab failed");
    if (++grabFailures >= MAX_GRAB_FAILURES) {
      Serial.println("FATAL: camera stopped delivering frames, restarting");
      delay(100);
      ESP.restart();
    }
    delay(100);
    return;
  }
  grabFailures = 0;
  int w, h;
  uint8_t *img = quirc_begin(qr, &w, &h);
  if (fb->width != (size_t)w || fb->height != (size_t)h || fb->len < (size_t)(w * h)) {
    esp_camera_fb_return(fb);
    return;
  }
  memcpy(img, fb->buf, (size_t)w * h);
  esp_camera_fb_return(fb);
  int64_t tGrabbed = esp_timer_get_time();
  usGrab += tGrabbed - tStart;

  // Take the preview before quirc_end(), which binarises the image in place.
  uint32_t now = millis();
  bool previewDue = previewOn && now - lastPreviewMs >= PREVIEW_INTERVAL_MS && previewFits();
  if (previewDue) {
    makePreview(img, w, h);
    lastPreviewMs = now;
  }

  int64_t tEnd = esp_timer_get_time();
  quirc_end(qr);
  int64_t tEnded = esp_timer_get_time();
  usEnd += tEnded - tEnd;
  statFrames++;

  int count = quirc_count(qr);
  for (int i = 0; i < count; i++) {
    quirc_extract(qr, i, &code);
    quirc_decode_error_t err = quirc_decode(&code, &data);
    if (err == QUIRC_ERROR_DATA_ECC) { // maybe a mirror image
      quirc_flip(&code);
      err = quirc_decode(&code, &data);
    }
    if (err) {
      statDecodeErrors++;
      continue;
    }
    statDecoded++;
    sendPayload(data.payload, (size_t)data.payload_len);
  }
  usDecode += esp_timer_get_time() - tEnded;

  if (previewDue) sendPreview(count);

  now = millis();
  if (redLedOffAt && (int32_t)(now - redLedOffAt) >= 0) {
    digitalWrite(RED_LED_GPIO, HIGH);
    redLedOffAt = 0;
  }

  if (now - statSince >= 5000) {
    uint32_t n = statFrames ? statFrames : 1;
    Serial.printf("[stats] %.1f fps, decoded %lu, sent %lu, decode errors %lu, stack free %u, "
                  "ms/frame grab %.1f end %.1f decode %.1f\n",
                  statFrames * 1000.0f / (now - statSince), (unsigned long)statDecoded,
                  (unsigned long)statSent, (unsigned long)statDecodeErrors,
                  (unsigned)uxTaskGetStackHighWaterMark(NULL),
                  usGrab / 1000.0f / n, usEnd / 1000.0f / n, usDecode / 1000.0f / n);
    statFrames = statDecoded = statSent = statDecodeErrors = 0;
    usGrab = usEnd = usDecode = 0;
    statSince = now;
  }
}
