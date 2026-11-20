# PC checks

These are small Node.js scripts with no dependencies. They work out expected values independently, so you can check the device's output against something other than the device's own code. Only use them with public test data.

| Script | What it does | Example |
|---|---|---|
| `fingerprint.js` | BIP39 test phrase + passphrase → master fingerprint (PBKDF2-HMAC-SHA512, HMAC "Bitcoin seed", secp256k1, hash160) | `node fingerprint.js "Air Gap #1 \o/"` → `94ae7c6c`, or `73c5da0a` with no passphrase |
| `addr_check.js` | Checks the bech32 checksums of two addresses, and derives `/0/0` and `/1/0` from a tpub (P2WPKH, `tb`) to compare | `node addr_check.js <tpub> <receive0> <change0>` |
| `xpub_check.js` | Decodes an extended public key and shows its base58 checksum, version, depth, parent fingerprint and child number | `node xpub_check.js <tpub>` |
| `desc_checksum.js` | BIP380 descriptor checksum (a port of Bitcoin Core's). Self-tests on `raw(deadbeef)#89f8spxm` and Core's `pkh(...)#ml40v0wf` | `node desc_checksum.js "wpkh([...]tpub.../<0;1>/*)"` |
| `kb_check.js` | Reads the on-screen keyboard layout (`KB_LAYOUT` in `ui.cpp`) and checks it has 4 pages of 28 keys covering all 95 printable ASCII characters | `node kb_check.js ../../airgap_wallet/ui.cpp` |
| `electrum_probe.js` | Read-only check of an Electrum server that fetches `server.version` and the latest block | `node electrum_probe.js <host> 50002` |
| `pow_check.js` | Checks that the server's latest block header and the 4 before it meet their proof-of-work target, and that block 1 builds on the signet genesis block. Useful if Sparrow says a header "does not meet its claimed proof of work target" | `node pow_check.js <host> 50002` |

`fingerprint.js` uses the public BIP39 test phrase (`abandon` ×11 + `about`). Never pass it a real seed.

## Related tools

- `../cam_viewer/serial_capture.js` logs a board's serial output to a file.
- `../cam_viewer/tests/` holds offline tests for `viewer.js` and `link_check.js`.
- `../link_fuzz/` is the camera-link fuzzer.
