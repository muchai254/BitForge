#include "touch.h"
#include "config.h"
#include "display.h"
#include "lock.h"
#include <TouchScreen.h>

static TouchScreen ts(PIN_TOUCH_XP, PIN_TOUCH_YP, PIN_TOUCH_XM, PIN_TOUCH_YM, TOUCH_RX_PLATE_OHMS);

static TouchCal cal = {
  TOUCH_SWAP_XY != 0, TOUCH_RAW_LEFT, TOUCH_RAW_RIGHT, TOUCH_RAW_TOP, TOUCH_RAW_BOTTOM
};

void touchBegin() {
  analogReadResolution(10); // TouchScreen hard-codes 10-bit maths (1023/1024)
}

void touchSetCalibration(const TouchCal &c) { cal = c; }
const TouchCal &touchCalibration() { return cal; }

// The touch measurement leaves some of the shared pins as inputs or driven
// to arbitrary levels. Put all four back as outputs, with CS deselected, so
// the next display write works.
static void restoreDisplayPins() {
  pinMode(PIN_TOUCH_XP, OUTPUT);
  pinMode(PIN_TOUCH_YM, OUTPUT);
  pinMode(PIN_TOUCH_YP, OUTPUT);
  pinMode(PIN_TOUCH_XM, OUTPUT);
  digitalWrite(PIN_TOUCH_XM, HIGH); // LCD_CS idle
}

bool touchReadRaw(int16_t &rx, int16_t &ry, int16_t &rz) {
  TSPoint p = ts.getPoint();
  restoreDisplayPins();
  rx = p.x;
  ry = p.y;
  rz = p.z;
  bool touched = p.z > TOUCH_MIN_Z && p.z < TOUCH_MAX_Z;
  if (touched) lockNoteActivity(); // resets the idle timeout
  return touched;
}

bool touchRead(int16_t &x, int16_t &y) {
  int16_t rx, ry, rz;
  if (!touchReadRaw(rx, ry, rz)) return false;
  int16_t ax = cal.swapXY ? ry : rx; // raw axis that follows screen x
  int16_t ay = cal.swapXY ? rx : ry;
  x = constrain(map(ax, cal.rawLeft, cal.rawRight, 0, tft.width()), 0, tft.width() - 1);
  y = constrain(map(ay, cal.rawTop, cal.rawBottom, 0, tft.height()), 0, tft.height() - 1);
  return true;
}

TouchEvent touchPoll() {
  static bool down = false;
  static uint8_t pressCount = 0;
  static uint32_t lastSeen = 0;
  static int16_t px = 0, py = 0;

  int16_t x, y;
  if (touchRead(x, y)) {
    lastSeen = millis();
    if (!down && ++pressCount >= 3) {
      down = true;
      px = x;
      py = y;
      return {TOUCH_PRESS, px, py};
    }
  } else {
    pressCount = 0;
    if (down && millis() - lastSeen > 50) {
      down = false;
      return {TOUCH_RELEASE, px, py};
    }
  }
  return {TOUCH_NONE, 0, 0};
}
