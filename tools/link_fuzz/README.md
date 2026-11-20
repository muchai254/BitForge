# Camera-link fuzzer

Use this to check that the wallet survives bad data on the camera link. It turns the ESP32-CAM into a fuzzer that sends a mix of the following at about 11.5 KB/s.

- valid `#QR` / `#QB` frames
- random garbage bytes
- frames with one flipped bit
- wrong CRCs
- cut-off frames followed by valid ones
- oversized lengths
- a 5,000-byte line with no end
- format errors

Every 10 seconds the CAM's debug console prints `[fuzz] items N, valid frames sent V, ...`.

The test passes if the wallet's Scan test screen never crashes, its free heap stays flat, and its `ok` count keeps up with `V`.

## How to run it

The fuzzer isn't part of the normal camera firmware, so you add it by hand.

1. Copy `link_fuzz.cpp` and `link_fuzz.h` into `airgap_qrcam/`.
2. Make three changes in `airgap_qrcam.ino`.
   - add `#include "link_fuzz.h"`
   - call `fuzzBegin()` at the end of `setup()`
   - in `loop()`, right after `handleConsole();`, add `fuzzStep(); return;`
3. Flash the CAM and watch the wallet's Scan test screen.
4. When you're done, remove the two files and the three lines, then flash the normal firmware again.
