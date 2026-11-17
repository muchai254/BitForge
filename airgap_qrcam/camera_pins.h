// AI-Thinker ESP32-CAM (OV2640) pin map.
// Copied from the esp32 core 3.3.10 CameraWebServer example (camera_pins.h,
// CAMERA_MODEL_AI_THINKER).
#pragma once

#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27

#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

#define FLASH_LED_GPIO 4   // bright white flash LED
#define RED_LED_GPIO   33  // small red LED on the back, active low

// One-way link to the wallet: our TX only. GPIO14 is free because the SD
// card slot isn't used.
#define LINK_TX_GPIO   14
#define LINK_BAUD      115200
