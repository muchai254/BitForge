#include "calibrate.h"
#include "display.h"

static const int M = 20; // crosshair distance from the screen edges

static void drawCross(int x, int y, uint16_t color) {
  tft.drawFastHLine(x - 10, y, 21, color);
  tft.drawFastVLine(x, y - 10, 21, color);
  tft.drawCircle(x, y, 6, color);
}

struct RawSample {
  long x, y;
  int16_t zMin, zMax;
};

// Waits for a press on one crosshair, averages the readings while it is
// held, and retries (flashing the cross red) until at least 5 good samples
// were collected.
static RawSample sampleTarget(int cx, int cy) {
  while (true) {
    long sx = 0, sy = 0;
    int n = 0;
    int16_t zMin = INT16_MAX, zMax = INT16_MIN;
    int16_t rx, ry, rz;

    while (!touchReadRaw(rx, ry, rz)) delay(5);
    delay(30); // let the first-contact noise settle
    uint32_t lastSeen = millis();
    while (millis() - lastSeen < 150) {
      if (touchReadRaw(rx, ry, rz)) {
        sx += rx;
        sy += ry;
        n++;
        zMin = min(zMin, rz);
        zMax = max(zMax, rz);
        lastSeen = millis();
      }
      delay(5);
    }
    Serial.printf("  samples=%d z=%d..%d\n", n, zMin, zMax);
    if (n >= 5) return {sx / n, sy / n, zMin, zMax};

    drawCross(cx, cy, COL_RED);
    delay(300);
    drawCross(cx, cy, COL_BTC_ORANGE);
  }
}

TouchCal runTouchCalibration() {
  const int W = tft.width(), H = tft.height();
  const int tx[4] = {M, W - M, M, W - M};
  const int ty[4] = {M, M, H - M, H - M};

  while (true) {
    long rawX[4], rawY[4];
    int16_t zMin = INT16_MAX, zMax = INT16_MIN;

    tft.fillScreen(COL_BLACK);
    drawCentredText("Touch calibration", 120, 2, COL_WHITE);
    drawCentredText("Press and hold each", 150, 1, COL_GREY);
    drawCentredText("orange cross, then lift", 165, 1, COL_GREY);
    Serial.println("touch calibration: start");

    for (int i = 0; i < 4; i++) {
      drawCross(tx[i], ty[i], COL_BTC_ORANGE);
      RawSample s = sampleTarget(tx[i], ty[i]);
      rawX[i] = s.x;
      rawY[i] = s.y;
      zMin = min(zMin, s.zMin);
      zMax = max(zMax, s.zMax);
      drawCross(tx[i], ty[i], COL_GREEN);
      Serial.printf("target %d screen(%d,%d) raw x=%ld y=%ld\n", i, tx[i], ty[i], rawX[i], rawY[i]);
    }

    // Moving across the screen (left -> right) should mostly change one raw
    // axis. If it's raw y, the axes are swapped.
    long dxx = labs((rawX[1] + rawX[3]) - (rawX[0] + rawX[2]));
    long dxy = labs((rawY[1] + rawY[3]) - (rawY[0] + rawY[2]));
    bool swapXY = dxy > dxx;
    long *forX = swapXY ? rawY : rawX;
    long *forY = swapXY ? rawX : rawY;

    // Extrapolate from the crosshairs (M px in) to the real screen edges.
    float left = (forX[0] + forX[2]) / 2.0f, right = (forX[1] + forX[3]) / 2.0f;
    float top = (forY[0] + forY[1]) / 2.0f, bottom = (forY[2] + forY[3]) / 2.0f;
    float perPxX = (right - left) / (W - 2 * M);
    float perPxY = (bottom - top) / (H - 2 * M);
    TouchCal c;
    c.swapXY = swapXY;
    c.rawLeft = (int16_t)(left - perPxX * M);
    c.rawRight = (int16_t)(right + perPxX * M);
    c.rawTop = (int16_t)(top - perPxY * M);
    c.rawBottom = (int16_t)(bottom + perPxY * M);

    // Sanity checks, otherwise a corner was mis-read and we start again:
    // - each axis must span a reasonable part of the ADC range;
    // - corners on the same column must agree on x, and corners on the same
    //   row must agree on y (within a quarter of the span).
    long spanX = labs(c.rawRight - c.rawLeft), spanY = labs(c.rawBottom - c.rawTop);
    bool sane = spanX > 200 && spanY > 200 &&
                labs(forX[0] - forX[2]) < spanX / 4 && labs(forX[1] - forX[3]) < spanX / 4 &&
                labs(forY[0] - forY[1]) < spanY / 4 && labs(forY[2] - forY[3]) < spanY / 4;

    Serial.println("--- paste into config.h ---");
    Serial.println("#define TOUCH_CAL_VALID 1");
    Serial.printf("#define TOUCH_SWAP_XY   %d\n", swapXY ? 1 : 0);
    Serial.printf("#define TOUCH_RAW_LEFT  %d\n", c.rawLeft);
    Serial.printf("#define TOUCH_RAW_RIGHT %d\n", c.rawRight);
    Serial.printf("#define TOUCH_RAW_TOP   %d\n", c.rawTop);
    Serial.printf("#define TOUCH_RAW_BOTTOM %d\n", c.rawBottom);
    Serial.printf("// pressure seen: z=%d..%d\n", zMin, zMax);
    Serial.printf("calibration %s\n", sane ? "OK" : "REJECTED (corners inconsistent), retrying");

    if (sane) {
      tft.fillScreen(COL_BLACK);
      drawCentredText("Calibrated", 20, 2, COL_GREEN);
      char buf[40];
      int y = 60;
      snprintf(buf, sizeof buf, "SWAP_XY %d", swapXY ? 1 : 0);   drawCentredText(buf, y, 2, COL_WHITE); y += 25;
      snprintf(buf, sizeof buf, "LEFT    %d", c.rawLeft);        drawCentredText(buf, y, 2, COL_WHITE); y += 25;
      snprintf(buf, sizeof buf, "RIGHT   %d", c.rawRight);       drawCentredText(buf, y, 2, COL_WHITE); y += 25;
      snprintf(buf, sizeof buf, "TOP     %d", c.rawTop);         drawCentredText(buf, y, 2, COL_WHITE); y += 25;
      snprintf(buf, sizeof buf, "BOTTOM  %d", c.rawBottom);      drawCentredText(buf, y, 2, COL_WHITE); y += 25;
      snprintf(buf, sizeof buf, "z %d..%d", zMin, zMax);         drawCentredText(buf, y, 2, COL_GREY);
      delay(2500);
      return c;
    }

    tft.fillScreen(COL_BLACK);
    drawCentredText("Didn't look right,", 140, 2, COL_RED);
    drawCentredText("let's try again", 165, 2, COL_RED);
    delay(2000);
  }
}
