#!/usr/bin/env bash
# Short Kestrel-RT walkthrough on the physical UNO R4 WiFi (about 1 minute; 2 with --esp).
# A demo, not evidence: the measured campaigns are in docs/TEST_PLAN.md and docs/PHASE2_RESULTS.md.
#   scripts/demo.sh            board check, build, flash only if the board differs, timing + one fault
#   scripts/demo.sh --esp      also crash the ESP32-S3 and show the RA4M1 unaffected (Phase 2 fork on
#                              the ESP; moves the host Wi-Fi to the ESP SoftAP; set KX_HOME_WIFI to return)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
APP=${DEMO_APP:-phase2}
echo "== board"
lsusb | grep -q '2341:1002' || { echo "UNO R4 WiFi (2341:1002) not found"; exit 2; }
lsusb | grep '2341:1002'
echo "== build APP=$APP"
make -s APP="$APP" >/dev/null
sha256sum "build/$APP/kestrel.bin"
echo "== board image vs build (read-only SWD)"
if python3 scripts/swd_verify.py app "build/$APP/kestrel.bin" >/dev/null 2>&1; then
  echo "board already runs this image"
else
  echo "board differs: flashing through the Arduino bootloader (scripts/flash.sh)"
  make -s flash APP="$APP"
  sleep 3
fi
ARGS=(--elf "build/$APP/kestrel.elf")
if [ "${1:-}" = --esp ]; then scripts/esp_wifi.sh up >/dev/null; ARGS+=(--esp); fi
rc=0; python3 host/demo.py "${ARGS[@]}" || rc=$?
if [ "${1:-}" = --esp ] && [ -n "${KX_HOME_WIFI:-}" ]; then scripts/esp_wifi.sh down >/dev/null || true; fi
exit $rc
