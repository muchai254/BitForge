# Wiring

This page shows how to connect the three parts:
- **A.** the 2.8" TFT touch shield to the ESP32 DevKit
- **B.** the ESP32-CAM to the DevKit (the camera link)
- **C.** a USB-TTL adapter to the ESP32-CAM, for flashing it

If you change a pin, change it in `airgap_wallet/config.h` too. The two must match.

## A. TFT shield → ESP32 DevKit

The shield is made to sit on an Arduino Uno. Here you connect it to the DevKit with jumper wires instead. **Unplug the DevKit from USB while you wire.**

| Shield pin (Uno label) | What it does | DevKit GPIO | Notes |
|---|---|---|---|
| LCD_D0 (D8) | data bit 0, also touch YM | **22** | Not the library default (12). See below. Needs [patch 5](patches.md) |
| LCD_D1 (D9) | data bit 1, also touch XP | 13 | |
| LCD_D2 (D2) | data bit 2 | 26 | |
| LCD_D3 (D3) | data bit 3 | 25 | |
| LCD_D4 (D4) | data bit 4 | 17 | |
| LCD_D5 (D5) | data bit 5 | 16 | |
| LCD_D6 (D6) | data bit 6 | 27 | |
| LCD_D7 (D7) | data bit 7 | 14 | |
| LCD_RD (A0) | read | **21** | Not the library default (2). See below. Needs [patch 4](patches.md) |
| LCD_WR (A1) | write | 4 | |
| LCD_RS (A2) | command/data, also touch YP (analog) | 15 | ADC2 channel 3 |
| LCD_CS (A3) | chip select, also touch XM (analog) | 33 | ADC1 channel 5 |
| LCD_RST (A4) | reset | 32 | |
| 5V | power | 5V / VIN | |
| 3V3 | power | 3V3 | |
| GND | ground | GND | |

Leave the shield's SD-card pins (D10–D13) unconnected.

**Why two pins differ from the library defaults:**
- **GPIO12** is a "strapping" pin, which the ESP32 reads at power-on to choose settings. If the shield pulls it high, the ESP32 sets its flash to 1.8 V and keeps rebooting. Use GPIO22 instead.
- **GPIO2** is also a strapping pin. If it's used, the DevKit can't enter upload mode by itself. Use GPIO21 instead.

**Uploading to the DevKit:** hold the **BOOT** button for the whole upload, release it when the upload finishes, then press **EN** to run. On many cheap DevKits the automatic reset into upload mode is unreliable: it releases EN before GPIO0 is pulled low. A 1–10 µF capacitor between EN and GND should fix this, but it hasn't been tested.

## B. ESP32-CAM → ESP32 DevKit (the camera link)

The camera sends scanned QR data to the wallet over **one wire, in one direction only**. There is **no wire from the DevKit to the CAM**. The DevKit's GPIO34 is an input-only pin, so the wallet physically can't send anything back.

### Tested setup: CAM powered by the USB-TTL adapter

This is the setup used during development.

| From | To | Why |
|---|---|---|
| CAM IO14 | DevKit GPIO34 | the data link (115200 baud, 8N1) |
| CAM GND | DevKit GND | the two boards need a common ground |
| Adapter 5V | CAM 5V | the CAM's **only** power supply. Don't also connect DevKit 5V to the CAM |
| Adapter GND | CAM GND | |
| Adapter RX | CAM U0T (GPIO1) | the CAM's debug console (2,000,000 baud) |
| Adapter TX | CAM U0R (GPIO3) | for flashing and console commands. ⚠ See the 5V warning in C |

### Without the adapter (not yet tested)

When you no longer need the adapter, power the CAM from the DevKit:

| ESP32-CAM | DevKit | Notes |
|---|---|---|
| GPIO14 | GPIO34 | data, one way |
| 5V | 5V / VIN | if the CAM resets under load, give it its own 5V supply and keep GND shared |
| GND | GND | |

### Warnings
- ⚠ **Don't use the CAM pin labelled "GND/R" as ground.** On this board it's connected to the reset line. If you wire it to ground, the CAM stays stuck in reset: it shows no console output and scans nothing, and the wallet receives junk. Use the GND pin next to the 5V pin.
- **Never power the CAM from two 5V sources at once.**
- **GPIO34 has no internal pull-up resistor.** While the CAM is off, in reset or booting, the wallet receives random bytes. This is harmless: the wallet throws them away and counts them as "skipped". To keep the line quiet, you can add a 10 kΩ resistor from GPIO34 to 3V3.

## C. Flashing the ESP32-CAM (USB-TTL adapter)

The ESP32-CAM has no USB port. You program it through a USB-TTL adapter, a small USB-to-serial board.

| USB-TTL | ESP32-CAM |
|---|---|
| 5V | 5V |
| GND | GND |
| TX | U0R (GPIO3) |
| RX | U0T (GPIO1) |

**To flash:**
1. Connect GPIO0 to GND. This puts the CAM into upload mode.
2. Press RESET on the CAM.
3. Upload the sketch.
4. Remove the GPIO0–GND wire.
5. Press RESET again. The new firmware now runs.

Disconnect the CAM from the DevKit's 5V while it's on the adapter.

⚠ **5V logic warning.** Many cheap FT232 adapters have one jumper that sets both the supply voltage and the signal voltage. At 3.3V the supply is too weak for the CAM, so you have to set it to 5V. The adapter's TX pin then sends **5V signals into CAM GPIO3**, which is more than the ESP32 is rated for. Protect the pin:
- **Best:** adapter TX → 1 kΩ → CAM U0R, with a 2 kΩ resistor from CAM U0R to GND. This divides the signal down to about 3.3V.
- **At least:** a 1–2.2 kΩ resistor in series on that line.

## Free DevKit pins

After wiring A and B, these pins are still free:
- **Input and output:** 18, 19, 23
- **Input only:** 35, 36, 39

Avoid 0, 2, 5 and 12, which are strapping pins. Pin 15 is a strapping pin too, but it's already used for LCD_RS.
