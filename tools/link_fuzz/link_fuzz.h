// Link fuzz test for the CAM: floods the wallet link with valid frames mixed
// with garbage, corrupted, truncated and oversized frames. Copy into the CAM
// sketch only for testing; not part of normal builds.
#pragma once
#include <Arduino.h>

void fuzzBegin();
// Sends one fuzz item. Call repeatedly.
void fuzzStep();
