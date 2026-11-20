#pragma once
#include <Arduino.h>

struct Button {
  int16_t x, y, w, h;
  const char *label;
  uint16_t bg, fg;
  uint8_t textSize;
};

void drawButton(const Button &b, bool pressed);
bool buttonHit(const Button &b, int16_t x, int16_t y);

// Blocks until a button is pressed and released; returns its index.
// Presses outside every button are ignored.
int runButtons(const Button *buttons, int count);

// Draws text word-wrapped inside width w. Returns the y below the last line.
int drawTextWrapped(const char *text, int x, int y, int w, uint8_t size, uint16_t color);

// Shows a title and message with Reject / Approve buttons. Returns true only
// if Approve was tapped. Nothing is ever approved by default.
bool confirmScreen(const char *title, const char *message);

void messageScreen(const char *title, const char *message);

// Keyboard for all 95 printable ASCII characters (a BIP39 passphrase may use
// any). Writes up to cap-1 chars plus a terminator into out; returns the
// length when OK is tapped.
size_t keyboardInput(const char *prompt, char *out, size_t cap);
