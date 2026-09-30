#!/usr/bin/env bash
# Build the Kestrel ESP32-S3 bridge fork. Upstream sources come from the pinned official checkout
# (arduino/uno-r4-wifi-usb-bridge tag 0.5.2, commit 4394ba03) and are staged unmodified next to the
# fork's own files; the portable Kestrel protocol code (link/xlink.c, trace/crc16.c) is shared with the RA4.
# Output: build/esp/KestrelBridge.bin (+ .sha256). Flashing: scripts/flash_esp_fork.sh (ROM mode only).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REF=${KX_BRIDGE_REF:-$HOME/Projects/_ref/uno-r4-wifi-usb-bridge}
[ "$(git -C "$REF" rev-parse HEAD)" = 4394ba03c6bd1afb661da2944de68359ec5b415a ] || {
  echo "upstream checkout is not tag 0.5.2 (4394ba03...)"; git -C "$REF" rev-parse HEAD; exit 2; }
STAGE=$ROOT/build/esp/KestrelBridge
rm -rf "$STAGE"; mkdir -p "$STAGE"
for f in "$REF"/UNOR4USBBridge/*; do
  b=$(basename "$f"); [ "$b" = UNOR4USBBridge.ino ] || [ "$b" = build ] && continue; cp -r "$f" "$STAGE/"
done
cp "$ROOT"/esp/KestrelBridge/*.ino "$ROOT"/esp/KestrelBridge/*.cpp "$ROOT"/esp/KestrelBridge/*.h "$STAGE/"
sed 's#"link/xlink.h"#"xlink.h"#; s#"trace/crc16.h"#"crc16.h"#' "$ROOT/link/xlink.c" > "$STAGE/xlink.c"
cp "$ROOT/link/xlink.h" "$ROOT/trace/crc16.h" "$STAGE/"
cp "$ROOT/trace/crc16.c" "$STAGE/crc16.c"; sed -i 's#"trace/crc16.h"#"crc16.h"#' "$STAGE/crc16.c"
SECRETS=$ROOT/esp/secrets.env   # gitignored: KX_AP_SSID=..., KX_AP_PASS=...
if [ ! -f "$SECRETS" ]; then
  printf 'KX_AP_SSID=kestrel-esp32\nKX_AP_PASS=%s\n' "$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 20)" > "$SECRETS"
  chmod 600 "$SECRETS"
fi
. "$SECRETS"
printf '#pragma once\n#define KX_AP_SSID "%s"\n#define KX_AP_PASS "%s"\n' "$KX_AP_SSID" "$KX_AP_PASS" > "$STAGE/kx_secrets.h"
FQBN="esp32-patched:esp32:arduino_unor4wifi_usb_bridge:JTAGAdapter=default,PSRAM=disabled,FlashMode=qio,FlashSize=4M,LoopCore=1,EventsCore=1,USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,PartitionScheme=unor4wifi,CPUFreq=240,UploadSpeed=921600,DebugLevel=none,EraseFlash=none"
arduino-cli compile --config-file "$REF/arduino-cli.yaml" --fqbn="$FQBN" --output-dir "$ROOT/build/esp/out" "$STAGE" 2>&1 | tail -${KX_LOG_LINES:-6}
cp "$ROOT/build/esp/out/KestrelBridge.ino.bin" "$ROOT/build/esp/KestrelBridge.bin"
( cd "$ROOT/build/esp" && sha256sum KestrelBridge.bin > KestrelBridge.bin.sha256 && cat KestrelBridge.bin.sha256 )
