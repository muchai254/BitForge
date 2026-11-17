// Resistive touch on the shield's shared LCD pins.
#pragma once
#include <Arduino.h>

struct TouchCal {
  bool swapXY;
  int16_t rawLeft, rawRight, rawTop, rawBottom; // raw values at the screen edges
};

enum TouchEventType : uint8_t { TOUCH_NONE, TOUCH_PRESS, TOUCH_RELEASE };

struct TouchEvent {
  TouchEventType type;
  int16_t x, y; // screen coordinates where the finger first landed
};

void touchBegin();
void touchSetCalibration(const TouchCal &c);
const TouchCal &touchCalibration();

// One raw sample. Returns true if the pressure is in the accepted range.
// Always leaves the shared pins ready for drawing.
bool touchReadRaw(int16_t &rx, int16_t &ry, int16_t &rz);

// One calibrated sample in screen coordinates.
bool touchRead(int16_t &x, int16_t &y);

// Debounced events; call every ~10 ms. A press needs 3 consecutive readings
// (resistive panels are noisy on first contact); a release needs 50 ms with
// no touch.
TouchEvent touchPoll();
