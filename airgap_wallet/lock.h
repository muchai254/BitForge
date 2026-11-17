// Session end: "Wipe & lock" and the idle timeout.
//
// A separate FreeRTOS task does the locking, so that it can stop the UI task
// wherever it is (deep inside any screen) and wipe its whole stack, which a
// task can't do to itself. It then wipes the session, the camera link and the
// free heap, and restarts: the device comes back to the boot screen, which
// says why. Nothing is ever written to flash; the reason survives the restart
// in RTC memory (a plain number, not a secret).
#pragma once
#include <stdint.h>

enum LockReason : uint32_t {
  LOCK_NONE = 0,
  LOCK_IDLE = 0x1D1E0001,   // no touch for IDLE_LOCK_MS
  LOCK_MANUAL = 0x1D1E0002, // the user tapped Wipe & lock
};

// Starts the lock task. Call at the end of setup() (from the UI task).
void lockBegin();

// Called on every valid touch reading.
void lockNoteActivity();

// The main menu with no seed loaded has nothing to protect, so the idle
// timeout doesn't apply there. The main loop sets this around its menu.
void lockSetIdleExempt(bool exempt);

// Wipes everything and restarts. Never returns.
[[noreturn]] void lockNow();

// Why the previous restart happened (LOCK_NONE after a power-on or any other
// reset). Clears the record.
LockReason lockLastReason();
