#!/usr/bin/env bash
# Kestrel-RT walkthrough on the physical board (about 1 minute; about 2 with --esp). See docs/DEMO.md.
# Lines marked [live] are read from the board during the run. The closing VALIDATED block is read from
# recorded result files. The measured campaigns are the evidence, not this demo.
#   scripts/demo.sh            board check, build, flash only if the board differs, timing + one fault
#   scripts/demo.sh --esp      also crash the ESP32-S3 and show the RA4M1 unaffected (Phase 2 fork on
#                              the ESP; moves the host Wi-Fi to the ESP SoftAP; set KX_HOME_WIFI to return)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
APP=${DEMO_APP:-phase2}
echo "KESTREL-RT  bare-metal runtime, Renesas RA4M1 (Arm Cortex-M4F, 48 MHz) + ESP32-S3 network domain"
echo
echo "-- board -------------------------------------------------------------"
lsusb | grep -q '2341:1002' || { echo "UNO R4 WiFi (2341:1002) not found"; exit 2; }
echo "  [live] USB $(lsusb | grep '2341:1002' | cut -d' ' -f6-)"
make -s APP="$APP" >/dev/null
echo "  build  APP=$APP  kestrel.bin $(sha256sum "build/$APP/kestrel.bin" | cut -c1-16)..."
if python3 scripts/swd_verify.py app "build/$APP/kestrel.bin" >/dev/null 2>&1; then
  echo "  [live] flash at 0x4000 matches the build byte for byte (read-only SWD)"
else
  echo "  board differs: flashing through the Arduino bootloader (scripts/flash.sh)"
  make -s flash APP="$APP"
  sleep 3
fi
ARGS=(--elf "build/$APP/kestrel.elf")
if [ "${1:-}" = --esp ]; then scripts/esp_wifi.sh up >/dev/null; ARGS+=(--esp); fi
rc=0; python3 host/demo.py "${ARGS[@]}" || rc=$?
if [ "${1:-}" = --esp ] && [ -n "${KX_HOME_WIFI:-}" ]; then scripts/esp_wifi.sh down >/dev/null || true; fi
exit $rc
