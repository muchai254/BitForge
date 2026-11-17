#include "display.h"
#include "config.h"
#include <math.h>
#include <string.h>

MCUFRIEND_kbv tft;

uint16_t displayBegin() {
  uint16_t id = tft.readID();
  tft.begin(id);
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(COL_BLACK);
  return id;
}

void drawCentredText(const char *text, int y, uint8_t size, uint16_t color) {
  int w = (int)strlen(text) * 6 * size;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor((tft.width() - w) / 2, y);
  tft.print(text);
}

// --- Bitcoin logo ---
// Shape helpers work in "logo units": the circle has radius 100, y points down.
static bool inRect(float x, float y, float x0, float y0, float x1, float y1) {
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static bool inCircle(float x, float y, float cx, float cy, float r) {
  float dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

static bool insideB(float x, float y) {
  bool topOuter = inRect(x, y, -28, -42, 12, -2) || inCircle(x, y, 12, -22, 20);
  bool topHole  = inRect(x, y, -10, -26, 12, -18) || inCircle(x, y, 12, -22, 4);
  if (topOuter && !topHole) return true;

  bool botOuter = inRect(x, y, -28, -6, 16, 42) || inCircle(x, y, 16, 18, 24);
  bool botHole  = inRect(x, y, -10, 10, 16, 26) || inCircle(x, y, 16, 18, 8);
  if (botOuter && !botHole) return true;

  if (inRect(x, y, -36, -42, -28, -30) || inRect(x, y, -36, 30, -28, 42)) return true;
  if (inRect(x, y, -20, -54, -12, -42) || inRect(x, y, -2, -54, 6, -42)) return true;
  if (inRect(x, y, -20, 42, -12, 54) || inRect(x, y, -2, 42, 6, 54)) return true;
  return false;
}

void drawBitcoinLogo(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, COL_BTC_ORANGE);

  // Rotate each screen pixel back into the upright B and draw matching runs.
  const float a = 14.0f * (float)M_PI / 180.0f;
  const float c = cosf(a), s = sinf(a);
  const float unitsPerPixel = 100.0f / r;
  for (int dy = -r; dy <= r; dy++) {
    int runStart = 0;
    bool inRun = false;
    for (int dx = -r; dx <= r + 1; dx++) {
      bool on = false;
      if (dx <= r) {
        float lx = ( dx * c + dy * s) * unitsPerPixel;
        float ly = (-dx * s + dy * c) * unitsPerPixel;
        on = insideB(lx, ly);
      }
      if (on && !inRun) {
        runStart = dx;
        inRun = true;
      } else if (!on && inRun) {
        tft.drawFastHLine(cx + runStart, cy + dy, dx - runStart, COL_WHITE);
        inRun = false;
      }
    }
  }
}
