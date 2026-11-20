// Hardware and policy configuration for the BitForge wallet (ESP32 DevKit).
// Must match docs/wiring.md.
#pragma once

#define FIRMWARE_VERSION "0.8.0-phase8"

// --- TFT shield (8-bit parallel ILI9341) ---
// MCUFRIEND_kbv drives these itself (utility/mcufriend_shield.h, ESP32 block).
// They are repeated here for documentation and for the touch driver.
#define PIN_LCD_RD  21  // moved off GPIO2 (strapping pin); MCUFRIEND patch 4
#define PIN_LCD_WR   4
#define PIN_LCD_RS  15  // also touch YP (analog, ADC2_CH3)
#define PIN_LCD_CS  33  // also touch XM (analog, ADC1_CH5)
#define PIN_LCD_RST 32
#define PIN_LCD_D0  22  // also touch YM; moved off GPIO12 (MTDI strapping pin); MCUFRIEND patch 5
#define PIN_LCD_D1  13  // also touch XP
#define PIN_LCD_D2  26
#define PIN_LCD_D3  25
#define PIN_LCD_D4  17
#define PIN_LCD_D5  16
#define PIN_LCD_D6  27
#define PIN_LCD_D7  14

#define EXPECTED_LCD_ID 0x9341

// Portrait: 240 wide x 320 tall.
#define SCREEN_ROTATION 0

// --- Resistive touch (shares pins with the LCD; same mapping as on the Uno) ---
#define PIN_TOUCH_XP PIN_LCD_D1 // digital
#define PIN_TOUCH_YM PIN_LCD_D0 // digital
#define PIN_TOUCH_YP PIN_LCD_RS // analog
#define PIN_TOUCH_XM PIN_LCD_CS // analog
#define TOUCH_RX_PLATE_OHMS 300

// The touch library assumes a 10-bit ADC, so analogReadResolution(10) is
// used and all raw values below are 0..1023.
// Set TOUCH_CAL_VALID to 1 after pasting values from the calibration screen.
// Until then the firmware runs calibration at every boot.
#define TOUCH_CAL_VALID 1
#define TOUCH_SWAP_XY   1
#define TOUCH_RAW_LEFT  177
#define TOUCH_RAW_RIGHT 931
#define TOUCH_RAW_TOP   977
#define TOUCH_RAW_BOTTOM 171
#define TOUCH_MIN_Z 10
#define TOUCH_MAX_Z 1000

// --- Session ---
// No touch for this long: everything is wiped and the device restarts.
#define IDLE_LOCK_MS (5UL * 60UL * 1000UL)

// --- Camera link: ESP32-CAM TX -> this RX, one-way ---
// GPIO34 is input-only, so this pin can never drive the camera.
#define PIN_CAM_RX  34
