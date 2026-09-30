#!/usr/bin/env bash
# Upload a Kestrel image to THIS UNO R4 WiFi through the Arduino bootloader (bossac, SAM-BA on SCI9).
# Board-specific: bridge automatic bootloader entry is unreliable on this board, so the bootloader's
# own double-tap handshake is performed over SWD (scripts/uno_swd_bootloader_entry.py, proven in the
# author's earlier projects on this board). Flash is NEVER written over SWD.
# Gates: image check (no byte below 0x4000), bootloader block verified read-only before and after,
# application verified read-only after upload.
# Usage: scripts/flash.sh build/<app>/kestrel.bin
set -euo pipefail
BIN=${1:?kestrel.bin}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ELF=${BIN%.bin}.elf
python3 "$ROOT/scripts/check_image.py" "$ELF"
B=$(ls -d ~/.arduino15/packages/arduino/tools/bossac/*/ | head -1)bossac
lsusb | grep -q '2341:1002' || { echo "UNO R4 WiFi bridge (2341:1002) not visible"; exit 2; }
for n in /dev/bus/usb/001/* /dev/hidraw*; do sudo -n /usr/bin/chmod 666 "$n" 2>/dev/null || true; done
PORT=$(for d in /sys/class/tty/ttyACM*; do u=$d/device/..; [ "$(cat $u/idVendor 2>/dev/null)" = 2341 ] && [ "$(cat $u/idProduct)" = 1002 ] && basename $d; done | head -1 || true)
[ -n "$PORT" ] || { echo "no UNO R4 WiFi tty"; exit 2; }
echo "== pre: bootloader block (read-only)"
python3 "$ROOT/scripts/swd_verify.py" boot
python3 "$ROOT/scripts/uno_swd_bootloader_entry.py" || { echo "bootloader entry failed"; exit 3; }
stty -F "/dev/$PORT" 230400   # SAM-BA rate; never 1200 (reset trigger) or 2400 (ROM boot trigger)
"$B" -d --port="$PORT" -U -e -w "$BIN" -R 2>&1 | grep -E 'Done|error|Error|No device|pages' || true
sleep 2
echo "== post: bootloader block + application (read-only)"
python3 "$ROOT/scripts/swd_verify.py" boot app "$BIN"
echo "flashed $(sha256sum "$BIN" | cut -d' ' -f1) to /dev/$PORT"
