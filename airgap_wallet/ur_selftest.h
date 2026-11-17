#pragma once

// Dev tests -> "UR self-test": runs the UR decoder (src/ur) against the
// upstream bc-ur vectors, a lossy stream, hostile inputs and real Sparrow
// captures (ur_selftest_vectors.h, made by tools/ur/gen_selftest.js).
// Logs each check to Serial, shows PASS/FAIL, waits for Back.
void urSelfTestScreen();
