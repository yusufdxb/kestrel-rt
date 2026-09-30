#!/usr/bin/env bash
# Restore the ESP32-S3 on THIS UNO R4 WiFi to the official Arduino USB bridge firmware.
#
# Requires the ESP32-S3 in ROM download mode (USB 303a:1001 "USB JTAG/serial debug unit"):
#   - if a bridge (stock or Kestrel fork) is running:  python3 scripts/esp_rom_mode.py
#     (HID feature report 0xAA, the same mechanism as Arduino's unor4wifi-reboot)
#   - if no firmware responds: jumper ESP_DOWNLOAD to GND on the ESP header, then unplug/replug USB.
#
# Images (hashes checked before anything is written):
#   official : Arduino release 0.5.2 UNOR4-WIFI-S3-0.5.2.bin written at 0x0 (1,966,080 bytes)
#   factory  : the full 8 MB raw backup of this board taken 2026-09-30 (byte-identical to the
#              official image over its length; also restores spiffs/nvs contents)
# Usage: scripts/restore_stock_esp_bridge.sh [official|factory]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-official}
OFFICIAL=$ROOT/build/esp/u052/unor4wifi-update-linux/firmware/UNOR4-WIFI-S3-0.5.2.bin
OFFICIAL_SHA=a488aacfe614fb77afe339862884f971f8058b447c11eb0e458f1666010cd7ca
FACTORY=$ROOT/backups/esp32_factory_20260930T171352Z.bin
FACTORY_SHA=603807cbe3e0de90f31e29e4aa3ad8e46099b03dba83b18d3eb59b285232a4e6
export PATH=$HOME/.local/bin:$PATH
case $MODE in
  official) IMG=$OFFICIAL; SHA=$OFFICIAL_SHA ;;
  factory)  IMG=$FACTORY;  SHA=$FACTORY_SHA ;;
  *) echo "usage: $0 [official|factory]"; exit 2 ;;
esac
[ -f "$IMG" ] || { echo "image missing: $IMG (official: re-fetch with scripts/fetch_esp_stock.sh)"; exit 3; }
GOT=$(sha256sum "$IMG" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "sha256 mismatch for $IMG: $GOT"; exit 4; }
lsusb | grep -q '303a:1001' || { echo "ESP32-S3 not in ROM download mode (303a:1001 absent). Nothing written."; exit 5; }
PORT=$(for d in /sys/class/tty/ttyACM*; do u=$d/device/..; [ "$(cat $u/idVendor 2>/dev/null)" = 303a ] && basename $d; done | head -1)
sudo -n /usr/bin/chmod 666 "/dev/$PORT" 2>/dev/null || true
esptool.py --chip esp32s3 -p "/dev/$PORT" --before no_reset --after no_reset flash_id | grep -E "Chip is|flash size"
esptool.py --chip esp32s3 -p "/dev/$PORT" --before no_reset --after no_reset write_flash 0x0 "$IMG"
esptool.py --chip esp32s3 -p "/dev/$PORT" --before no_reset --after no_reset verify_flash 0x0 "$IMG"
esptool.py --chip esp32s3 -p "/dev/$PORT" --before no_reset --after hard_reset chip_id >/dev/null
for i in $(seq 1 30); do sleep 1; lsusb | grep -q '2341:1002' && { echo "bridge back (2341:1002)"; exit 0; }; done
echo "restore written and verified, but the bridge did not re-enumerate within 30 s: unplug/replug USB"; exit 6
