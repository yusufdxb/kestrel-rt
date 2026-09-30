# ESP32-S3 bridge fork (Phase 2)

The ESP32-S3 on the UNO R4 WiFi runs a fork of Arduino's USB bridge 0.5.2. It keeps the USB CDC bridge,
CMSIS-DAP, the 1200/2400-baud handling and the 0xAA ROM-mode entry, fixes a RESET pulse that the stock
bridge sends to the RA4M1 at every ESP boot, and replaces the AT-command server with the Kestrel mailbox
node (`KestrelBridge/kx_node.cpp`). Design: docs/PHASE2_ARCHITECTURE.md, docs/PHASE2_PROTOCOL.md.

The upstream sketch has no license (docs/LICENSING.md), so it is not in this repository. The build
regenerates the validated sketch from your own upstream checkout:

| File | What it is |
|---|---|
| `patches/KestrelBridge.ino.ed` | `diff -e` script: upstream `UNOR4USBBridge.ino` -> `KestrelBridge.ino` (Kestrel lines only) |
| `KestrelBridge/kx_node.{h,cpp}` | Kestrel mailbox node: sessions, heartbeats, SoftAP, UDP, fault modes (MIT) |
| `build.sh` | stages upstream + ed script + Kestrel files, checks both sketch hashes, compiles |

## Build
Upstream environment, per the upstream README at tag 0.5.2 (about 3.3 GB with the toolchain):
```bash
git clone https://github.com/arduino/uno-r4-wifi-usb-bridge && cd uno-r4-wifi-usb-bridge
git checkout 4394ba03c6bd1afb661da2944de68359ec5b415a        # tag 0.5.2
git submodule update --init --depth 1 --no-single-branch
(cd hardware/esp32-patched/esp32/tools && ./get.py)
# point arduino-cli.yaml's directories.user at this checkout
```
Then, from the Kestrel-RT root:
```bash
KX_BRIDGE_REF=/path/to/uno-r4-wifi-usb-bridge esp/build.sh              # build/esp/KestrelBridge.bin
KX_BRIDGE_REF=/path/to/uno-r4-wifi-usb-bridge esp/build.sh --show-diff  # readable diff vs upstream
```
The first run writes a random SoftAP password to the gitignored `esp/secrets.env`.
The ESP32-S3 build is not bit-reproducible: a fresh compile changes the embedded ELF hash and the image
digest (65 bytes) but no code or data byte. Compare a rebuild with the validated image
(sha256 `f1a96a1c...bdd4`, read back from the board) using `scripts/esp_image_compare.py`. The SoftAP
password from `esp/secrets.env` is compiled in, so a build with a different password also differs in
those bytes.
These setup steps are upstream's procedure. The release was verified against an existing checkout
prepared that way; a fresh clone-and-setup was not repeated for the release.

## Flash / restore
Only through ESP ROM download mode, app partition only: `python3 scripts/esp_rom_mode.py && scripts/flash_esp_fork.sh`.
Back to the official firmware: `scripts/restore_stock_esp_bridge.sh official` (docs/PHASE2_RECOVERY.md).
