#!/usr/bin/env bash
# Fetch third-party kernels used only by the baselines (not vendored; build/ext is gitignored).
# Pinned by tag AND verified by commit hash.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXT="$ROOT/build/ext"
mkdir -p "$EXT"
FR_TAG=V11.1.0
FR_COMMIT=dbf70559b27d39c1fdb68dfb9a32140b6a6777a0
if [ ! -d "$EXT/FreeRTOS-Kernel" ]; then
  git clone -q --depth 1 -b "$FR_TAG" https://github.com/FreeRTOS/FreeRTOS-Kernel "$EXT/FreeRTOS-Kernel"
fi
GOT=$(git -C "$EXT/FreeRTOS-Kernel" rev-parse HEAD)
echo "FreeRTOS-Kernel $FR_TAG @ $GOT"
[ "$GOT" = "$FR_COMMIT" ] || { echo "commit mismatch (expected $FR_COMMIT)"; exit 1; }
