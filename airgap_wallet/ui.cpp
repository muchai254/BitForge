#include "ui.h"
#include "display.h"
#include "touch.h"
#include <string.h>

#define COL_KEY      0x4208
#define COL_KEY_FN   0xA514
#define COL_REJECT   0xC000
#define COL_APPROVE  0x0560
#define COL_FIELD_BG 0x10A2

void drawButton(const Button &b, bool pressed) {
  uint16_t bg = pressed ? b.fg : b.bg;
  uint16_t fg = pressed ? b.bg : b.fg;
  tft.fillRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 6, bg);
  int tw = (int)strlen(b.label) * 6 * b.textSize;
  int th = 8 * b.textSize;
  tft.setTextSize(b.textSize);
  tft.setTextColor(fg);
  tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2);
  tft.print(b.label);
}

bool buttonHit(const Button &b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

int runButtons(const Button *buttons, int count) {
  int held = -1;
  while (true) {
    TouchEvent e = touchPoll();
    if (e.type == TOUCH_PRESS) {
      held = -1;
      for (int i = 0; i < count; i++) {
        if (buttonHit(buttons[i], e.x, e.y)) { held = i; break; }
      }
      if (held >= 0) drawButton(buttons[held], true);
    } else if (e.type == TOUCH_RELEASE && held >= 0) {
      drawButton(buttons[held], false);
      return held;
    }
    delay(10);
  }
}

int drawTextWrapped(const char *text, int x, int y, int w, uint8_t size, uint16_t color) {
  const int cw = 6 * size, lh = 10 * size;
  const int perLine = max(1, w / cw);
  tft.setTextSize(size);
  tft.setTextColor(color);
  const char *p = text;
  while (*p) {
    // Take as many whole words as fit on this line.
    int len = (int)strlen(p);
    int take = min(len, perLine);
    if (take < len) {
      int cut = take;
      while (cut > 0 && p[cut] != ' ') cut--;
      if (cut > 0) take = cut;
    }
    tft.setCursor(x, y);
    for (int i = 0; i < take; i++) tft.print(p[i]);
    p += take;
    while (*p == ' ') p++;
    y += lh;
  }
  return y;
}

bool confirmScreen(const char *title, const char *message) {
  tft.fillScreen(COL_BLACK);
  drawCentredText(title, 16, 2, COL_BTC_ORANGE);
  drawTextWrapped(message, 12, 56, tft.width() - 24, 2, COL_WHITE);
  const Button buttons[2] = {
    {8, 250, 108, 60, "Reject", COL_REJECT, COL_WHITE, 2},
    {124, 250, 108, 60, "Approve", COL_APPROVE, COL_WHITE, 2},
  };
  for (const Button &b : buttons) drawButton(b, false);
  return runButtons(buttons, 2) == 1;
}

void messageScreen(const char *title, const char *message) {
  tft.fillScreen(COL_BLACK);
  drawCentredText(title, 16, 2, COL_BTC_ORANGE);
  drawTextWrapped(message, 12, 56, tft.width() - 24, 2, COL_WHITE);
  const Button ok = {70, 262, 100, 46, "OK", COL_GREY, COL_BLACK, 2};
  drawButton(ok, false);
  runButtons(&ok, 1);
}

// 7 x 4 character keys plus a control row:
//   [Shift] [123] [  space  ] [DEL] [OK]
// Pages: letters (Shift toggles case), "123" (digits + common symbols),
// "#+=" (the remaining symbols).
static const int KB_COLS = 7, KB_ROWS = 4;
static const int KB_TOP = 140, KB_KEY_W = 34, KB_KEY_H = 35;
static const int KB_LEFT = (240 - KB_COLS * KB_KEY_W) / 2;
static const int KB_CTRL_Y = KB_TOP + KB_ROWS * KB_KEY_H;
static const int KB_CHARS = KB_COLS * KB_ROWS;

enum KbPage { KB_LOWER, KB_UPPER, KB_SYM1, KB_SYM2 };
// 28 characters per page; a space means "no key here".
static const char KB_LAYOUT[4][KB_CHARS + 1] = {
    "abcdefghijklmnopqrstuvwxyz  ",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ  ",
    "0123456789!@#$%&*()-_=+.,?/:",
    "\"';<>[]\\^`{|}~              ", // \\ is the backslash key
};
enum { K_SHIFT = KB_CHARS, K_PAGE, K_SPACE, K_DEL, K_OK, KB_TOTAL };

// Draws the text with spaces made visible (a short grey bar at the
// baseline): a passphrase's trailing space would otherwise be invisible.
static void drawField(const char *prompt, const char *text) {
  tft.fillRect(0, 0, tft.width(), KB_TOP, COL_BLACK);
  drawCentredText(prompt, 12, 2, COL_BTC_ORANGE);
  tft.fillRoundRect(8, 50, tft.width() - 16, 60, 6, COL_FIELD_BG);
  // Size 3 fits 11 characters plus the cursor; longer text switches to size 2
  // and shows its end (17 characters).
  size_t len = strlen(text);
  bool big = len <= 11;
  const char *shown = big || len <= 17 ? text : text + (len - 17);
  int size = big ? 3 : 2, cw = 6 * size;
  int x = 18, y = big ? 69 : 73;
  tft.setTextSize(size);
  tft.setTextColor(COL_WHITE);
  for (const char *p = shown; *p; p++, x += cw) {
    if (*p == ' ') {
      tft.fillRect(x + size, y + 7 * size - size, 4 * size, size, COL_GREY);
    } else {
      tft.setCursor(x, y);
      tft.print(*p);
    }
  }
  tft.setCursor(x, y);
  tft.print('_');
  // Length, so a hidden prefix (long text) is still accounted for.
  char n[16];
  snprintf(n, sizeof n, "%u", (unsigned)len);
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY);
  tft.setCursor(tft.width() - 12 - 6 * (int)strlen(n), 98);
  tft.print(n);
}

static void layoutKeys(Button *keys, char labels[KB_CHARS][2], KbPage page) {
  for (int i = 0; i < KB_CHARS; i++) {
    labels[i][0] = KB_LAYOUT[page][i];
    labels[i][1] = 0;
    int16_t x = KB_LEFT + (i % KB_COLS) * KB_KEY_W, y = KB_TOP + (i / KB_COLS) * KB_KEY_H;
    bool used = labels[i][0] != ' ';
    // An empty slot gets zero size: it can't be hit or drawn.
    keys[i] = {x, y, (int16_t)(used ? KB_KEY_W : 0), (int16_t)(used ? KB_KEY_H : 0), used ? labels[i] : "",
               COL_KEY, COL_WHITE, 2};
  }
  bool letters = page == KB_LOWER || page == KB_UPPER;
  const char *modeLabel = letters ? (page == KB_UPPER ? "abc" : "ABC") : (page == KB_SYM1 ? "#+=" : "123");
  keys[K_SHIFT] = {1, (int16_t)KB_CTRL_Y, 48, 38, modeLabel, COL_KEY_FN, COL_BLACK, 1};
  keys[K_PAGE] = {49, (int16_t)KB_CTRL_Y, 48, 38, letters ? "123" : "abc", COL_KEY_FN, COL_BLACK, 1};
  keys[K_SPACE] = {97, (int16_t)KB_CTRL_Y, 66, 38, "space", COL_KEY, COL_WHITE, 1};
  keys[K_DEL] = {163, (int16_t)KB_CTRL_Y, 38, 38, "DEL", COL_KEY_FN, COL_BLACK, 1};
  keys[K_OK] = {201, (int16_t)KB_CTRL_Y, 38, 38, "OK", COL_APPROVE, COL_WHITE, 2};
}

size_t keyboardInput(const char *prompt, char *out, size_t cap) {
  static char labels[KB_CHARS][2];
  Button keys[KB_TOTAL];
  KbPage page = KB_LOWER;
  size_t len = 0;
  out[0] = '\0';

  tft.fillScreen(COL_BLACK);
  bool redrawKeys = true;
  while (true) {
    if (redrawKeys) {
      layoutKeys(keys, labels, page);
      tft.fillRect(0, KB_TOP, tft.width(), tft.height() - KB_TOP, COL_BLACK);
      for (const Button &k : keys) {
        if (k.label[0]) drawButton(k, false);
      }
      redrawKeys = false;
    }
    drawField(prompt, out);
    int k = runButtons(keys, KB_TOTAL);
    char c = 0;
    if (k < KB_CHARS) {
      c = KB_LAYOUT[page][k];
      if (c == ' ') c = 0; // empty slot
    } else if (k == K_SPACE) {
      c = ' ';
    } else if (k == K_SHIFT) {
      page = page == KB_LOWER ? KB_UPPER : page == KB_UPPER ? KB_LOWER : page == KB_SYM1 ? KB_SYM2 : KB_SYM1;
      redrawKeys = true;
    } else if (k == K_PAGE) {
      page = (page == KB_LOWER || page == KB_UPPER) ? KB_SYM1 : KB_LOWER;
      redrawKeys = true;
    } else if (k == K_DEL) {
      if (len > 0) out[--len] = '\0';
    } else if (k == K_OK) {
      return len;
    }
    if (c && len + 1 < cap) {
      out[len++] = c;
      out[len] = '\0';
    }
  }
}
