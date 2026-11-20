# Protocols

This page covers how data moves through the system. It explains how the ESP32-CAM sends scanned QR codes to the wallet, which QR formats Sparrow uses, and the SeedQR formats you can use to back up a seed.

## 1. Camera → wallet frames

The sender is in `airgap_qrcam/link_frame.*` and the receiver is in `airgap_wallet/camlink.*`.

### The wire

The link is a UART running at **115200 baud**, 8N1. It works in one direction only, from CAM GPIO14 (TX) to DevKit GPIO34 (RX). The CAM has no RX pin assigned and there is no wire back.

Nothing else goes on this line. There are no boot messages and no debug text. The CAM sends its debug output to its other serial port (UART0).

### Frame format

Each scanned QR becomes one line.

```
#QR,<len>,<crc>,<payload>\n     when every payload byte is printable ASCII (0x20..0x7E), sent as is
#QB,<len>,<crc>,<base64>\n      otherwise, standard base64 (RFC 4648, '+/' alphabet, '=' padding, one line)
```

`<len>` is the length of the **raw** payload (before base64) in decimal, from 1 to **2953**. That's the most a QR code can hold (version 40-L, byte mode). It has no leading zeros or sign.

`<crc>` is exactly **8 lowercase hex digits**. It's the CRC-32/ISO-HDLC of the **raw** payload, the same CRC that zlib and Ethernet use.
- polynomial 0x04C11DB7, reflected (0xEDB88320)
- init 0xFFFFFFFF, final XOR 0xFFFFFFFF, input and output reflected
- **test vector: `"123456789"` → `cbf43926`**

A `#QR` payload can contain `#` or `,`. So split only on the first three commas, take exactly `<len>` bytes, and then expect `\n`.

### Receiving rules

Treat every byte from the camera as hostile.

To find frames, skip bytes until you see `#`. When you reject a frame, start looking again at the byte **after that frame's `#`**, not after the next `\n`. A cut-off frame can run straight into the next one (`#QR,5,…,he#QR,5,…,after\n`), so a good frame may start inside the bad line. A `#` that doesn't start a valid header is just a payload byte. Skip it, and don't count it as a rejected frame.

Reject the frame if any of these is true. Count it, then look for the next `#`.
- the type isn't `QR` or `QB`
- `len` is missing, has non-digits, is 0, or is more than 2953
- `crc` isn't 8 hex digits
- a `#QR` payload byte is outside 0x20..0x7E
- the base64 is invalid, or decodes to a length other than `len`
- the CRC doesn't match
- the `\n` at the end is missing

A line can be at most `13 + 4*ceil(2953/3) + 1` bytes long, which is about 4 KB. Drop any longer line.

You'll find a reference implementation, with tests for every rule above, in `tools/cam_viewer/link_check.js`.

### What the camera does

The camera doesn't repeat itself. While it keeps seeing the same payload (same length and CRC) within 1.5 s, it won't send it again. Each frame of an animated QR is different, so they all get through. The wallet must still cope with duplicates, which UR decoding does anyway.

For decoding, the camera runs quirc on grayscale frames. It uses QVGA (320×240) by default, and VGA is available from the debug console. If error correction fails, it tries once more on the mirror image.

## 2. QR formats used with Sparrow

These were tested with Sparrow 2.5.5 on signet.

| What | Direction | Format |
|---|---|---|
| Watch-only wallet | device → Sparrow | A **plain-text output descriptor**, `wpkh([fp/84h/1h/0h]tpub…/<0;1>/*)#checksum`. In Sparrow, go to New Wallet → Airgapped Hardware Wallet, pick a QR wallet entry (tested with "Era Wallet") and scan |
| Unsigned PSBT | Sparrow → device | An **animated UR**, `UR:CRYPTO-PSBT/<seq>-<count>/<bytewords>`. These are fountain-coded, so `seq` keeps counting past `count` |
| Signed PSBT | device → Sparrow | An animated UR in the same format |

**Set Sparrow's QR density to the lowest setting.** The small, sparse codes are much easier for the ESP32-CAM to read. In testing, a 1-input PSBT came out as 8 small frames that the camera read at about 0.93 frames/s. At the default density it read only about 0.09 frames/s.

### Input amounts and `non_witness_utxo`

To check the fee, the wallet must know how much each input is worth. A PSBT can carry this in two ways. A `witness_utxo` holds just the amount and script, and the computer could lie about it. A `non_witness_utxo` holds the whole previous transaction. The wallet can check that against the txid, so it can be trusted.

The wallet applies a simple rule. With 2 or more inputs, every input must have a `non_witness_utxo`, or the PSBT is refused. That's because with several inputs, false amounts can hide a huge fee (CVE-2020-14199). With 1 input, a `witness_utxo` alone is accepted, and the review screen notes that the fee couldn't be independently checked.

Sparrow decides whether to include `non_witness_utxo` based on the wallet type, not a setting, and its QR output usually leaves it out. For spends with more than one input, you may need to save the PSBT to a file and show it with `tools/psbt_show`, which includes it.

## 3. SeedQR

The wallet can show and scan your seed as a QR code, using the same formats as SeedSigner. The code is in `airgap_wallet/seedqr.*`.

⚠ **The QR code *is* your seed. Never photograph a real one.**

| Format | What's inside | QR (ECC L) | 12 words | 24 words |
|---|---|---|---|---|
| Standard SeedQR | each word's BIP39 index as 4 digits, joined together (numeric mode) | 48 / 96 digits | version 2 (25×25) | version 3 (29×29) |
| CompactSeedQR | the raw entropy, without the checksum bits (byte mode) | 16 / 32 bytes | version 1 (21×21) | version 2 (25×25) |

The device shows CompactSeedQR by default (Show words → last page → QR). Tap **Format** to switch to Standard. When you load a seed, the device tells the formats apart by length (48/96 digits vs 16/32 bytes) and then checks the BIP39 checksum.

As a test vector, take the public test phrase `abandon` ×11 + `about`. Its Standard SeedQR is `0000` ×11 + `0003`, and its Compact SeedQR is 16 zero bytes.

Over the camera link, a Standard SeedQR is sent as a `#QR` frame (digits are printable) and a Compact one as a `#QB` frame.

**SeedQR-shaped data is never echoed or logged**, by the camera's console or by the wallet, not even as a CRC. A CRC-32 of a 32-byte seed would reveal 32 bits of it.

After scanning a SeedQR, the wallet wipes its receive buffers, the UART receive buffer and the free heap (`camlinkWipe()`).
