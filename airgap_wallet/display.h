// Display wrapper around MCUFRIEND_kbv (patched for ESP-IDF 5; see docs/patches.md).
#pragma once
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>

// RGB565 colours
#define COL_BLACK      0x0000
#define COL_WHITE      0xFFFF
#define COL_RED        0xF800
#define COL_GREEN      0x07E0
#define COL_BLUE       0x001F
#define COL_GREY       0x8410
#define COL_BTC_ORANGE 0xF483 // #F7931A

extern MCUFRIEND_kbv tft;

// Reads the controller ID, initialises the panel and applies SCREEN_ROTATION.
// Returns the ID that was read.
uint16_t displayBegin();

void drawBitcoinLogo(int cx, int cy, int r);

// Prints text horizontally centred on the screen at y.
void drawCentredText(const char *text, int y, uint8_t size, uint16_t color);
