#!/usr/bin/env bash
# Fetch the official Arduino UNO R4 WiFi bridge firmware 0.5.2 (github.com/arduino, not a mirror).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); mkdir -p "$ROOT/build/esp"; cd "$ROOT/build/esp"
curl -sSL -o unor4wifi-update-linux-0.5.2.zip https://github.com/arduino/uno-r4-wifi-usb-bridge/releases/download/0.5.2/unor4wifi-update-linux.zip
echo "8afea1b2242c28a6be90af012d50f164808e6958f69ab36ccef23f87edac229e  unor4wifi-update-linux-0.5.2.zip" | sha256sum -c
rm -rf u052 && unzip -q unor4wifi-update-linux-0.5.2.zip -d u052
echo "a488aacfe614fb77afe339862884f971f8058b447c11eb0e458f1666010cd7ca  u052/unor4wifi-update-linux/firmware/UNOR4-WIFI-S3-0.5.2.bin" | sha256sum -c
