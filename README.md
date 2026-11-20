# BitForge

BitForge is an air-gapped Bitcoin signing device you can build from common hobby boards: an **ESP32 DevKit**, an **ESP32-CAM** and a **2.8" touchscreen shield**. It works with [Sparrow Wallet](https://sparrowwallet.com) on your computer, and the two exchange data only through QR codes. There is no USB data, Wi-Fi or Bluetooth link between them.

> ⚠ **This is a prototype for signet, Bitcoin's test network. Never use it with real funds or a real seed.**
> It has no secure element and no secure boot, and the ESP32 has a radio (unused by the firmware, but present). The firmware is hard-coded to signet.

## How it works

1. Sparrow holds only your public keys (a "watch-only" wallet). It builds an unsigned transaction (a **PSBT**) and shows it as an animated QR code.
2. The ESP32-CAM scans the QR and sends the data to the wallet over **one wire, in one direction**. The wallet has no way to send data back to the camera.
3. The wallet works out the amounts, the fee and your change **by itself**, from the transaction data and your keys. It shows them on the touchscreen.
4. You approve or reject on the touchscreen. If you approve, the wallet signs and shows the signed transaction as an animated QR code.
5. Sparrow scans it with your webcam and broadcasts it.

The seed is never stored. You load it each time you start the device, by typing the words or scanning a SeedQR, and it's wiped from memory when you choose **Wipe & lock** or leave the device idle for 5 minutes.

## What's in this repo

```
airgap_wallet/        Firmware for the ESP32 DevKit (the signer)
  airgap_wallet.ino     main menu and screens
  config.h              pins and settings (must match docs/wiring.md)
  display.*, touch.*, calibrate.*, ui.*    screen, touch input, buttons and keyboard
  entropy.*, seedgen.*, seedload.*, seedqr.*, keys.*, session.*, lock.*, secure_mem.*
                        creating, loading, holding and wiping the seed
  descriptor.*, export_screen.*, address_screen.*
                        watch-only export to Sparrow, address check
  camlink.*, scan_screen.*                 receiving QR data from the camera
  psbt_scan.*, psbt_parse.*, psbt_review.*, psbt_review_screen.*, psbt_sign.*, psbt_flow.*
                        the signing pipeline: scan, parse, verify, review, sign
  qr_out.*              showing QR codes (static and animated)
  ur_selftest.*, psbt_selftest.*           on-device self-tests (+ generated *_vectors.h)
  src/ur/               UR (animated QR) encoder/decoder, ported from bc-ur
  src/qrgen/            QR code generator (vendored, see docs/patches.md §3)
  partitions.csv        flash layout (no core-dump partition, so no RAM is written to flash)

airgap_qrcam/         Firmware for the ESP32-CAM (the QR scanner)
  airgap_qrcam.ino      camera, QR decoding, debug console
  link_frame.*          the frame format sent to the wallet
  src/quirc/            QR decoder (vendored, see docs/patches.md §6)

docs/
  wiring.md             how to connect everything
  patches.md            library changes you need to apply
  protocol.md           camera link format, Sparrow QR formats, SeedQR

tools/                PC-side helpers (Node.js)
  cam_viewer/           live camera preview, link checker, serial logger, scan statistics
  link_fuzz/            turns the CAM into a fuzzer for the camera link
  pc_checks/            independent checks of fingerprints, addresses, descriptors
  psbt_fixtures/        builds the 82 test PSBTs for the on-device PSBT self-test
  psbt_show/            shows a saved PSBT file as an animated QR in your browser
  ur/                   UR decoder in JS, its tests, and the UR self-test generator
```

## What you need

| Part | Used for |
|---|---|
| ESP32 DevKit (ESP32-WROOM-32) | the signer |
| ESP32-CAM (AI-Thinker, OV2640) | the QR scanner |
| 2.8" TFT touch shield for Arduino Uno (ILI9341, 8-bit parallel, resistive touch) | screen and input |
| USB-TTL adapter (FT232, CH340 or CP2102) | flashing the ESP32-CAM |
| Jumper wires | connections |
| A computer with Sparrow Wallet and a webcam | building and broadcasting transactions |

Wire it up using [docs/wiring.md](docs/wiring.md).

## Build and flash

### 1. Install the tools
- [arduino-cli](https://arduino.github.io/arduino-cli/) (or the Arduino IDE)
- ESP32 board support **3.3.10**: `arduino-cli core install esp32:esp32@3.3.10`

### 2. Install the libraries
| Library | Version | How |
|---|---|---|
| MCUFRIEND_kbv | 3.0.0-Release | Library Manager, then apply patches 1, 4 and 5 |
| Adafruit GFX Library | 1.12.6 | Library Manager |
| Adafruit BusIO | 1.17.4 | Library Manager (needed by GFX) |
| Adafruit TouchScreen | 1.1.6 | Library Manager |
| uBitcoin | 0.2.0 + secret wiping | clone the [fork](https://github.com/muchai254/uBitcoin/tree/muchai254/secret_material_wiping) into your libraries folder, then apply patch 2 |

The patches are in [docs/patches.md](docs/patches.md). Without them the firmware won't compile, or the board won't boot reliably.

### 3. Flash the ESP32-CAM
Connect the USB-TTL adapter (see wiring.md §C), connect GPIO0 to GND, press RESET, then run:
```
arduino-cli compile --upload -p <PORT> --fqbn esp32:esp32:esp32cam airgap_qrcam
```
Remove the GPIO0 wire and press RESET.

### 4. Flash the DevKit
**Hold the BOOT button** for the whole upload. Release it when the upload finishes, then press EN.
```
arduino-cli compile --upload -p <PORT> --fqbn esp32:esp32:esp32 airgap_wallet
```
`arduino-cli upload` on its own doesn't recompile, so always use `compile --upload` after changing code.

`config.h` holds touch calibration values for the author's screen. Yours will differ: set `TOUCH_CAL_VALID` to 0, flash, and the wallet runs calibration at boot. Copy the values it shows into `config.h` and set `TOUCH_CAL_VALID` back to 1.

### 5. Use it with Sparrow (signet)
1. In Sparrow, switch to **signet**.
2. On the device, load a seed. For testing, use the public test phrase `abandon` ×11 + `about` or generate a new one. **Never use a seed that holds real funds.**
3. On the device choose **Export to Sparrow**, then scan the QR in Sparrow: New Wallet → Airgapped Hardware Wallet → pick a QR wallet entry (tested with "Era Wallet").
4. Get signet coins from a signet faucet.
5. Build a transaction in Sparrow and show it as a QR. **Set QR density to the lowest setting**, because the camera reads small codes much better.
6. On the device, choose **Sign PSBT**, scan it, check each review screen, and approve.
7. Scan the signed QR back into Sparrow and broadcast.

For a transaction with more than one input, Sparrow's QR leaves out data the device requires (see [protocol.md](docs/protocol.md#input-amounts-and-non_witness_utxo)). Save the PSBT to a file in Sparrow and show it with `node tools/psbt_show/show.js <file.psbt>`.

## Tests

**On the device** (Dev tests menu):
- **UR self-test:** UR decoder against the bc-ur vectors, hostile input and real Sparrow captures.
- **PSBT self-test:** 82 PSBTs, valid and malicious, each of which must get the expected verdict.
- **Key test vector:** derives the BIP84 test vectors and checks that no secret is left on the stack.

**On your computer** (Node.js 18 or later):
```
node tools/ur/test.js                          # UR decoder tests
cd tools/cam_viewer
node tests/link_check_test.js link_check.js     # camera-link frame parser
node tests/viewer_parser_test.js viewer.js     # camera console parser
node tests/reconnect_test.js                   # serial reconnect
cd ../..
node tools/pc_checks/fingerprint.js            # expect 73c5da0a
```
The camera tests use a stand-in for the serial port, so no board is needed.
`tools/psbt_show` and `tools/cam_viewer` need `npm install` in their folders first.

To rebuild the self-test data:
- `node tools/psbt_fixtures/make_fixtures.js` writes `airgap_wallet/psbt_selftest_vectors.h`.
- `node tools/ur/gen_selftest.js` writes `airgap_wallet/ur_selftest_vectors.h`. It needs Sparrow PSBT captures in `tools/ur/fixtures/`, which aren't included in this repo. Capture your own with `tools/cam_viewer/viewer.js --log`.

## Security rules the code follows

- **No stored seed.** Secrets are never written to flash, and the flash layout has no core-dump partition.
- **No secrets on serial output**, ever.
- **Everything from the camera is treated as hostile.** Every frame is checked, and PSBTs go through a strict parser written for BitForge.
- **The scanned PSBT is never handed to uBitcoin.** The wallet signs a minimal PSBT it rebuilds from values it has verified itself.
- **Secrets are wiped from RAM** after use: buffers, the stack and the free heap.
- **No radio code.** Wi-Fi and Bluetooth are never included.
- **Signet only.** Keys are always derived for signet.

## Licence

Code in this repo is under the [MIT licence](LICENSE). Some parts come from other projects and keep their own licences:

| Code | From | Licence |
|---|---|---|
| `airgap_qrcam/src/quirc/` | [quirc](https://github.com/dlbeer/quirc) | ISC |
| `airgap_wallet/src/qrgen/` | [QRCode](https://github.com/ricmoo/qrcode) | MIT |
| `airgap_wallet/src/ur/`, `tools/ur/ur.js` | ported from [bc-ur](https://github.com/BlockchainCommons/bc-ur) | BSD-2-Clause-Patent |
