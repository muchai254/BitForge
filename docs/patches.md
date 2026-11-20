# Library patches

The firmware needs a few small changes to the Arduino libraries it uses. For each one, this page shows what breaks without it, which file to change, and the exact change to make.

Your installed libraries live in `<sketchbook>/libraries/`. **If you update or reinstall a patched library from the Library Manager, the patch is lost, so re-apply it afterwards.** Before you patch a file, keep a copy of the original next to it as `<file>.orig`.

| # | Library | Where the change goes |
|---|---|---|
| 1 | MCUFRIEND_kbv 3.0.0-Release | your libraries folder |
| 2 | uBitcoin 0.2.0 | your libraries folder |
| 3 | QRCode 0.0.1 | already in this repo (`airgap_wallet/src/qrgen/`) |
| 4 | MCUFRIEND_kbv | your libraries folder |
| 5 | MCUFRIEND_kbv | your libraries folder |
| 6 | quirc | already in this repo (`airgap_qrcam/src/quirc/`) |
| 7 | uBitcoin | already in the [uBitcoin fork](https://github.com/muchai254/uBitcoin/tree/muchai254/secret_material_wiping) |

## 1. MCUFRIEND_kbv: add a missing header (ESP-IDF 5)

On esp32 core 3.3.10 the build fails with `error: 'GPIO' was not declared in this scope` in `write_8()` / `read_8()`. This happens because ESP-IDF 5 no longer includes `soc/gpio_struct.h` automatically.

Open `MCUFRIEND_kbv/utility/mcufriend_shield.h` and find the `#elif defined(ESP32)` block, around line 1025. Add the include right after it.

```c
#elif defined(ESP32)       //regular UNO shield on TTGO D1 R32 (ESP32)
+#include "soc/gpio_struct.h"
```

## 2. uBitcoin: add a missing header (ESP-IDF 5)

Without this patch the build fails with `error: implicit declaration of function 'esp_random'`. In ESP-IDF 5, `esp_random()` is declared in `esp_random.h`, not in `esp_system.h`.

Open `uBitcoin/src/utility/trezor/rand.c` and find the `#if defined(ESP_PLATFORM)` block, around line 33. Add the second include.

```c
  #include <esp_system.h>
+ #include <esp_random.h>
```

uBitcoin uses this random function for ECDSA blinding, and for `generateMnemonic(numWords)` when you give it no entropy. The firmware never calls `generateMnemonic(numWords)`. It collects its own entropy instead. ECDSA nonces are deterministic (RFC 6979) and don't use it either.

## 3. QRCode: name clash, so it's copied into the repo

`#include <qrcode.h>` picks up the ESP32 core's own `qrcode.h`, which gives you `'QRCode' was not declared`.

To avoid the clash, the library is copied into `airgap_wallet/src/qrgen/` as `qrgen.c` / `qrgen.h`. You don't need to install QRCode, and if it's already installed it's ignored.

The copy differs from the original in a few ways, which are also listed in `src/qrgen/VENDORED.txt`.
- The files and include guard are renamed (`__QRGEN_H_`).
- The `#pragma mark` lines are removed.
- **`qrcode_initBytes()` now rejects data that doesn't fit**, as well as versions outside 1..40. The original writes past a stack buffer instead.
- Always-false unsigned comparisons are removed from `qrcode_getModule()`.

## 4. MCUFRIEND_kbv: move LCD_RD from GPIO2 to GPIO21

Without this change, uploads fail with `Failed to connect to ESP32: No serial data received`. GPIO2 is a strapping pin. It must be low or floating for the ESP32 to enter upload mode, but the shield's RD line sits high.

Change the ESP32 block in `MCUFRIEND_kbv/utility/mcufriend_shield.h`.

```c
-#define LCD_RD  2  //LED
+#define LCD_RD 21
```

Then connect the shield's LCD_RD to **GPIO21** (see [wiring.md](wiring.md)). Keep in mind that this affects every ESP32 sketch that uses this copy of the library.

## 5. MCUFRIEND_kbv: move LCD_D0 from GPIO12 to GPIO22

Without this change, the board sometimes loops at boot with errors such as `boot:0x33`, `qio_mode: Failed to set QIE bit` and `invalid header: 0xffffffff`. The ESP32 reads GPIO12 at reset to choose the flash voltage. The shield pulls it high, so the ESP32 powers its flash at 1.8 V instead of 3.3 V and reads garbage.

Change the ESP32 block in `MCUFRIEND_kbv/utility/mcufriend_shield.h`.

```c
-#define LCD_D0 12
+#define LCD_D0 22
```

Then connect the shield's LCD_D0 to **GPIO22** and leave GPIO12 unconnected.

⚠ **Don't "fix" this by burning the flash-voltage eFuse. That can't be undone.**

## 6. quirc: use single-precision floats

The ESP32's floating-point unit only handles `float`. quirc uses `double` by default, which is slow on the ESP32.

quirc (commit `927d680`, ISC licence) is copied into `airgap_qrcam/src/quirc/`, so you don't need to install anything. Two changes were made, and they're also listed in `src/quirc/VENDORED.txt`.
1. `quirc_internal.h` defines `QUIRC_FLOAT_TYPE float` and `QUIRC_USE_TGMATH 1` before `#include "quirc.h"` (inside `#ifndef QUIRC_FLOAT_TYPE`).
2. `identify.c` defines `QUIRC_USE_TGMATH 1` before its `<tgmath.h>`/`<math.h>` choice.

## 7. uBitcoin: wipe secrets from RAM

Some uBitcoin functions leave seeds and private keys in stack memory after they return. This patch wipes them. It **only adds wiping** and doesn't change any results.

To get it, install uBitcoin from the fork, branch [`muchai254/secret_material_wiping`](https://github.com/muchai254/uBitcoin/tree/muchai254/secret_material_wiping), into your libraries folder. Then apply patch 2 on top.

| File | Function | What was left in memory | Fix |
|---|---|---|---|
| `src/HDWallet.cpp` | `HDPrivateKey::fromMnemonic()` | `seed[64]` (BIP39 seed), `u[64]` (PBKDF2 block) | wiped before return |
| `src/HDWallet.cpp` | `HDPrivateKey::fromSeed()` | `raw[64]` (master key + chain code) | wiped |
| `src/HDWallet.cpp` | `HDPrivateKey::child()` | `data[37]` (parent private key for hardened steps), `raw[64]` | wiped |
| `src/Bitcoin.cpp` | `mnemonicToEntropy()` | `res[33]` (entropy) | wiped on both exits |
| `src/utility/trezor/bip39.c` | `mnemonic_to_entropy()` | `current_word[10]`, `bits[33]` | one exit that wipes both |
| `src/utility/trezor/bip39.c` | `mnemonic_check()` | `bits[33]` | one exit that wipes it |
| `src/utility/trezor/options.h` | `USE_BIP39_CACHE` | a static cache of mnemonic, passphrase and seed | turned off (`0`) |
| `src/utility/trezor/memzero.c` | `memzero()` | used `memset()`, which the compiler may remove | volatile byte loop |

The firmware adds its own layer of safety on top. `secure_scrub_stack()` in `airgap_wallet/secure_mem.cpp` overwrites the unused stack after key work. This also catches copies the patch can't reach, such as numbers held inside the elliptic-curve maths.

Two things are not covered by the patch. The firmware doesn't use either of them.
- `generateMnemonic(...)` leaves `hash[32]` behind.
- `PSBT::sign(const HDPrivateKey root)` copies the root key. The copy's destructor wipes it.

## Library versions

| Library | Version | Source |
|---|---|---|
| MCUFRIEND_kbv | 3.0.0-Release + patches 1, 4, 5 | https://github.com/prenticedavid/MCUFRIEND_kbv |
| uBitcoin | 0.2.0 from the fork (patch 7) + patch 2 | https://github.com/muchai254/uBitcoin (upstream: https://github.com/micro-bitcoin/uBitcoin) |
| QRCode | 0.0.1, in this repo (patch 3) | https://github.com/ricmoo/qrcode |
| quirc | commit `927d680`, in this repo (patch 6) | https://github.com/dlbeer/quirc |
| bc-ur | ported to C++ in `airgap_wallet/src/ur/` | https://github.com/BlockchainCommons/bc-ur |
