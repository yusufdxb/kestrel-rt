#!/usr/bin/env bash
# Write the Kestrel ESP32-S3 bridge fork to app0 (0x50000) ONLY. Bootloader, partition table,
# otadata (seq 1 -> app0), cert, spiffs and nvs are left untouched (the fork's bootloader and
# partition table are byte-identical to the board's; checked against the raw backup).
# Precondition: the recovery gate (docs/PHASE2_RECOVERY.md) and ROM download mode (303a:1001):
#   python3 scripts/esp_rom_mode.py
# Rollback: scripts/restore_stock_esp_bridge.sh official
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$ROOT/build/esp/KestrelBridge.bin}
export PATH=$HOME/.local/bin:$PATH
[ -f "$ROOT/backups/esp32_factory_20260930T171352Z.bin" ] || { echo "raw backup missing: recovery gate not satisfied"; exit 2; }
SZ=$(stat -c %s "$BIN"); [ "$SZ" -le $((0x190000)) ] || { echo "image larger than app0"; exit 3; }
python3 - "$BIN" <<'PY' || exit 4
import sys; d=open(sys.argv[1],"rb").read(); assert d[0]==0xE9, "not an ESP app image"
PY
lsusb | grep -q '303a:1001' || { echo "ESP32-S3 not in ROM download mode (303a:1001 absent). Nothing written."; exit 5; }
PORT=$(for d in /sys/class/tty/ttyACM*; do u=$d/device/..; [ "$(cat $u/idVendor 2>/dev/null)" = 303a ] && basename $d; done | head -1)
sudo -n /usr/bin/chmod 666 "/dev/$PORT" 2>/dev/null || true
E="esptool.py --chip esp32s3 -p /dev/$PORT --before no_reset"
$E --after no_reset flash_id | grep -E "Chip is|flash size"
$E --after no_reset write_flash 0x50000 "$BIN"
$E --after no_reset verify_flash 0x50000 "$BIN"
$E --after hard_reset chip_id >/dev/null
for i in $(seq 1 30); do sleep 1; lsusb | grep -q '2341:1002' && { echo "fork running (2341:1002), $(sha256sum "$BIN" | cut -d' ' -f1)"; exit 0; }; done
echo "written and verified, but 2341:1002 did not enumerate within 30 s"; exit 6
