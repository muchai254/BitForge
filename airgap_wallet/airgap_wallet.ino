// BitForge: ESP32 DevKit wallet firmware (signet only).
// Serial output is debug-only and never contains secrets.
#include <esp_system.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "calibrate.h"
#include "ui.h"
#include "camlink.h"
#include "scan_screen.h"
#include "keys.h"
#include "seedgen.h"
#include "seedload.h"
#include "export_screen.h"
#include "address_screen.h"
#include "ur_selftest.h"
#include "psbt_flow.h"
#include "psbt_selftest.h"
#include "lock.h"
#include "session.h"
#include "secure_mem.h"

// The default 8 KB loop stack is too small for key derivation, the stack
// leak check, and PSBT parsing and signing.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_EXT:      return "external (EN button)";
    default:               return "other";
  }
}

static void bootScreen(uint16_t id) {
  tft.fillScreen(COL_BLACK);
  drawBitcoinLogo(tft.width() / 2, 130, 95);
  drawCentredText("BitForge", 245, 2, COL_WHITE);
  drawCentredText("SIGNET PROTOTYPE", 270, 1, COL_BTC_ORANGE);
  char buf[32];
  snprintf(buf, sizeof buf, "fw %s  LCD 0x%04X", FIRMWARE_VERSION, id);
  drawCentredText(buf, 300, 1, COL_GREY);
}

// Checks touch accuracy up to the edges; the press counter exposes ghost touches.
static void touchTestScreen() {
  const Button back = {150, 280, 84, 36, "Back", COL_GREY, COL_BLACK, 2};
  tft.fillScreen(COL_BLACK);
  drawCentredText("Touch test", 6, 2, COL_BTC_ORANGE);
  for (int x = 0; x < tft.width(); x += 40) tft.drawFastVLine(x, 30, 245, 0x2104);
  for (int y = 30; y < 275; y += 40) tft.drawFastHLine(0, y, tft.width(), 0x2104);
  drawButton(back, false);

  uint32_t presses = 0;
  bool wasDown = false;
  uint32_t lastSeen = 0;
  while (true) {
    int16_t x, y;
    if (touchRead(x, y)) {
      lastSeen = millis();
      if (!wasDown) {
        wasDown = true;
        presses++;
        if (buttonHit(back, x, y)) {
          // Wait for release so the menu doesn't see this press.
          while (millis() - lastSeen < 100) {
            int16_t a, b;
            if (touchRead(a, b)) lastSeen = millis();
            delay(10);
          }
          return;
        }
      }
      if (y > 30 && y < 275) tft.fillCircle(x, y, 2, COL_GREEN);
      char buf[40];
      snprintf(buf, sizeof buf, "x=%3d y=%3d  presses=%lu ", x, y, (unsigned long)presses);
      tft.fillRect(0, 282, 146, 30, COL_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(COL_WHITE);
      tft.setCursor(4, 286);
      tft.print(buf);
    } else if (wasDown && millis() - lastSeen > 50) {
      wasDown = false;
    }
    delay(10);
  }
}

static void keyboardTestScreen() {
  char text[33]; // long enough to try the size-2 view (> 11 characters)
  size_t n = keyboardInput("Type anything", text, sizeof text);
  // Length only: people may type real passphrases on test screens too.
  Serial.printf("keyboard demo: %u chars typed\n", (unsigned)n);
  tft.fillScreen(COL_BLACK);
  drawCentredText("You typed:", 80, 2, COL_GREY);
  drawTextWrapped(n ? text : "(nothing)", 12, 110, 216, 2, COL_WHITE);
  secure_zero(text, sizeof text);
  delay(3000);
}

static void confirmTestScreen() {
  bool ok = confirmScreen("Confirm test",
                          "Tap Approve or Reject. In the real wallet this screen comes "
                          "after the transaction review.");
  Serial.printf("confirm demo: %s\n", ok ? "APPROVED" : "REJECTED");
  tft.fillScreen(COL_BLACK);
  drawCentredText(ok ? "Approved" : "Rejected", 145, 3, ok ? COL_GREEN : COL_RED);
  delay(1500);
}

static void keyTestVectorScreen() {
  keysRunTestVector();
  Serial.printf("[keys] loop stack: %u bytes free at the low point since the last scrub\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);
  runButtons(&back, 1);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nBitForge %s\n", FIRMWARE_VERSION);
  Serial.printf("reset reason: %s\n", resetReasonName(esp_reset_reason()));

  uint16_t id = displayBegin();
  Serial.printf("LCD ID: 0x%04X %s\n", id, id == EXPECTED_LCD_ID ? "(OK)" : "(UNEXPECTED)");
  touchBegin();
  camlinkBegin();
  Serial.printf("camera link: RX on GPIO%d, no TX\n", PIN_CAM_RX);

  LockReason lastLock = lockLastReason();
  if (lastLock != LOCK_NONE) {
    Serial.printf("restarted after %s (seed wiped)\n", lastLock == LOCK_IDLE ? "idle timeout" : "wipe & lock");
  }

  bootScreen(id);
  delay(1500);

  if (!TOUCH_CAL_VALID) {
    Serial.println("no touch calibration in config.h, running calibration");
    touchSetCalibration(runTouchCalibration());
  }
  lockBegin();
  lockSetIdleExempt(true); // nothing to protect on the "Locked" message
  showLockReason(lastLock);
  lockSetIdleExempt(false);
}

static void devTestsMenu() {
  const Button menu[8] = {
    {20, 38, 200, 30, "PSBT self-test", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 73, 200, 30, "UR self-test", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 108, 200, 30, "QR loopback", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 143, 200, 30, "Keyboard test", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 178, 200, 30, "Confirm test", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 213, 200, 30, "Touch test", COL_BTC_ORANGE, COL_BLACK, 2},
    {20, 248, 200, 30, "Recalibrate", COL_GREY, COL_BLACK, 2},
    {20, 283, 200, 30, "Back", COL_GREY, COL_BLACK, 2},
  };
  tft.fillScreen(COL_BLACK);
  drawCentredText("Dev tests", 12, 2, COL_WHITE);
  for (const Button &b : menu) drawButton(b, false);
  int choice = runButtons(menu, 8);
  Serial.printf("menu: %s\n", menu[choice].label);
  switch (choice) {
    case 0: psbtSelfTestScreen(); break;
    case 1: urSelfTestScreen(); break;
    case 2: qrLoopbackTestScreen(); break;
    case 3: keyboardTestScreen(); break;
    case 4: confirmTestScreen(); break;
    case 5: touchTestScreen(); break;
    case 6: touchSetCalibration(runTouchCalibration()); break;
  }
}

static void wipeAndLockScreen() {
  if (!confirmScreen("Wipe & lock?", "Erase the seed from memory and restart. You'll need your written words "
                     "or SeedQR to load it again."))
    return;
  Serial.println("[lock] wipe & lock requested");
  Serial.flush();
  tft.fillScreen(COL_BLACK);
  drawCentredText("Wiping...", 140, 3, COL_GREEN);
  lockNow(); // the lock task wipes everything and restarts
}

static void showLockReason(LockReason r) {
  if (r == LOCK_NONE) return;
  tft.fillScreen(COL_BLACK);
  drawCentredText(r == LOCK_IDLE ? "Locked" : "Wiped & locked", 40, 3, COL_GREEN);
  drawTextWrapped(r == LOCK_IDLE ? "No touch for 5 minutes, so the seed was wiped from memory."
                                 : "The seed was wiped from memory.",
                  16, 100, 208, 2, COL_WHITE);
  drawTextWrapped("Load it again from your backup to continue.", 16, 190, 208, 1, COL_GREY);
  const Button ok = {70, 262, 100, 46, "OK", COL_GREY, COL_BLACK, 2};
  drawButton(ok, false);
  runButtons(&ok, 1);
}

enum MenuAction { M_SIGN, M_GENERATE, M_LOAD, M_ADDRESSES, M_SHOW_WORDS, M_EXPORT, M_WIPE, M_TEST_VECTOR, M_SCAN, M_DEV };

void loop() {
  Button menu[7];
  MenuAction action[7];
  int n = 0;
  auto add = [&](const char *label, uint16_t bg, MenuAction a) {
    menu[n] = {20, (int16_t)(40 + n * 39), 200, 34, label, bg, COL_BLACK, 2};
    action[n++] = a;
  };
  // With a seed loaded, it must be wiped before another is made or loaded.
  if (session.loaded) {
    add("Sign PSBT", COL_BTC_ORANGE, M_SIGN);
    add("Addresses", COL_BTC_ORANGE, M_ADDRESSES);
    add("Export to Sparrow", COL_BTC_ORANGE, M_EXPORT);
    add("Show words", COL_BTC_ORANGE, M_SHOW_WORDS);
    add("Wipe & lock", COL_RED, M_WIPE);
  } else {
    add("Generate seed", COL_BTC_ORANGE, M_GENERATE);
    add("Load seed", COL_BTC_ORANGE, M_LOAD);
    add("Key test vector", COL_GREY, M_TEST_VECTOR);
  }
  add("Scan test", COL_GREY, M_SCAN);
  add("Dev tests", COL_GREY, M_DEV);

  tft.fillScreen(COL_BLACK);
  if (session.loaded) {
    char fp[24];
    snprintf(fp, sizeof fp, "Seed %02x%02x%02x%02x", session.fingerprint[0], session.fingerprint[1],
             session.fingerprint[2], session.fingerprint[3]);
    drawCentredText(fp, 14, 2, COL_GREEN);
  } else {
    drawCentredText("No seed loaded", 14, 2, COL_WHITE);
  }
  for (int i = 0; i < n; i++) drawButton(menu[i], false);

  // Only this menu, with no seed loaded, is exempt from the idle timeout.
  lockSetIdleExempt(true);
  int choice = runButtons(menu, n);
  lockSetIdleExempt(false);
  Serial.printf("menu: %s\n", menu[choice].label);
  switch (action[choice]) {
    case M_SIGN: signPsbtScreen(); break;
    case M_GENERATE: generateSeedScreen(); break;
    case M_LOAD: loadSeedScreen(); break;
    case M_ADDRESSES: addressScreen(); break;
    case M_SHOW_WORDS: showWordsScreen(); break;
    case M_EXPORT: exportScreen(); break;
    case M_WIPE: wipeAndLockScreen(); break;
    case M_TEST_VECTOR: keyTestVectorScreen(); break;
    case M_SCAN: scanTestScreen(); break;
    case M_DEV: devTestsMenu(); break;
  }
}
