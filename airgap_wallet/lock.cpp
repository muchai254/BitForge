#include "lock.h"
#include <Arduino.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "camlink.h"
#include "config.h"
#include "secure_mem.h"
#include "session.h"

// Survives esp_restart() (not a power cycle). Only a reason code.
RTC_NOINIT_ATTR static uint32_t rtcLockReason;

static volatile uint32_t lastActivityMs = 0;
static volatile bool idleExempt = false;
static TaskHandle_t uiTask = nullptr;
static TaskHandle_t lockTask = nullptr;

void lockNoteActivity() { lastActivityMs = millis(); }
void lockSetIdleExempt(bool exempt) {
  idleExempt = exempt;
  lastActivityMs = millis();
}

// Runs in the lock task. Must not use anything the stopped UI task might be
// holding a lock on: no Serial (its mutex), no Serial2/HardwareSerial, no TFT.
[[noreturn]] static void wipeAndRestart(uint32_t reason) {
  vTaskSuspend(uiTask);
  // On the other core the UI task stops only when it leaves any critical
  // section it's in; wait until it really is suspended before touching its stack.
  for (int i = 0; i < 100 && eTaskGetState(uiTask) != eSuspended; i++) vTaskDelay(1);
  vTaskDelay(pdMS_TO_TICKS(10));

  uint8_t *stack = pxTaskGetStackStart(uiTask);
  size_t stackSize = getArduinoLoopTaskStackSize();
  if (stack) secure_zero(stack, stackSize); // every frame of every screen

  sessionWipe();
  camlinkZeroBuffers();
  uart_driver_delete(UART_NUM_2); // frees the camera link's RX ring buffer (Serial2)
  size_t heap = secure_scrub_free_heap();

  rtcLockReason = reason;
  esp_rom_printf("[lock] %s: UI task stopped; %u B of its stack, the session and the camera link wiped; "
                 "%u B of free heap zeroed; restarting\n",
                 reason == LOCK_IDLE ? "idle timeout" : "wipe & lock", (unsigned)stackSize, (unsigned)heap);
  esp_restart();
}

static void lockTaskFn(void *) {
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) wipeAndRestart(LOCK_MANUAL);
    bool protect = session.loaded || !idleExempt;
    if (protect && millis() - lastActivityMs > IDLE_LOCK_MS) wipeAndRestart(LOCK_IDLE);
  }
}

void lockBegin() {
  uiTask = xTaskGetCurrentTaskHandle();
  lastActivityMs = millis();
  // Higher priority than the UI (1), on the other core.
  xTaskCreatePinnedToCore(lockTaskFn, "lock", 4096, nullptr, 3, &lockTask, 0);
}

void lockNow() {
  xTaskNotifyGive(lockTask);
  for (;;) vTaskDelay(portMAX_DELAY); // the lock task stops this task
}

LockReason lockLastReason() {
  uint32_t r = rtcLockReason;
  rtcLockReason = LOCK_NONE;
  // RTC_NOINIT memory holds garbage after a power-on; trust it only after our own restart.
  if (esp_reset_reason() != ESP_RST_SW) return LOCK_NONE;
  if (r != LOCK_IDLE && r != LOCK_MANUAL) return LOCK_NONE;
  return (LockReason)r;
}
