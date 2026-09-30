# Phase 2 recovery gate (ESP32-S3)

Status: **PASSED 2026-09-30, read-only.** Nothing was written to the ESP32-S3 to pass it.

| Gate item | Evidence |
|---|---|
| Current bridge firmware | 0.5.2 (HID GET_FEATURE report 00 05 02), USB 2341:1002, serial B081849CBB4C |
| ROM download mode reachable | HID feature report 0xAA (Arduino's own unor4wifi-reboot mechanism) -> USB 303a:1001; esptool connects |
| Chip identity | ESP32-S3 QFN56 rev v0.2, 8 MB embedded GD flash (c8/4017), MAC b0:81:84:xx:xx:xx, no secure boot, no flash encryption (backups/MANIFEST.md) |
| Flash readable | full 8 MB read in 190.6 s |
| Full backup | backups/esp32_factory_20260930T171352Z.bin, sha256 603807cb...232a4e6, `verify_flash`: digest matched |
| Official stock image | github.com/arduino/uno-r4-wifi-usb-bridge release 0.5.2 (tag commit 4394ba03), UNOR4-WIFI-S3-0.5.2.bin sha256 a488aacf...10cd7ca; **byte-identical to the backup over its 1,966,080 bytes** |
| Restore prepared | scripts/restore_stock_esp_bridge.sh [official\|factory]: checks hashes, refuses unless 303a:1001 is present, writes, verifies, resets. Refusal path exercised (exit 5). |
| Exit from ROM mode | esptool hard reset -> stock bridge re-enumerated as 2341:1002; RA4 serial link and CMSIS-DAP back |

Fallback that needs no firmware at all: jumper ESP_DOWNLOAD to GND on the ESP header, unplug/replug USB,
then run the restore script. (Not exercised; no soldering required.)

## Finding during the gate
Leaving ROM mode rebooted the ESP32-S3 and **the RA4M1 was reset with it** (Kestrel status afterwards:
uptime ~3 s, reset cause pin/unknown + warm, VBATT boot counter continuous, so not a power loss). The stock
bridge's `setup()` switches the RA4 reset GPIO to output before driving it high, which can pulse the RA4 RES
line low. With stock firmware, every ESP reboot therefore also resets the real-time domain; Phase 2 must
remove that coupling (see docs/PHASE2_ARCHITECTURE.md) and prove it on hardware.

## Tools
- scripts/esp_rom_mode.py (HID 0xAA), scripts/fetch_esp_stock.sh (official image, hash-pinned),
  scripts/restore_stock_esp_bridge.sh, esptool 4.12.0 (pip, user install).
