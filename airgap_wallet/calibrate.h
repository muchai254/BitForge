#pragma once
#include "touch.h"

// Runs the interactive calibration, prints the config.h values to Serial,
// shows them on screen and returns them. Retries until the result looks sane.
TouchCal runTouchCalibration();
