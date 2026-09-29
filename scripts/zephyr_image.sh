#!/usr/bin/env bash
# Build the Zephyr baseline and derive a bossac-safe image: the option-setting/ID-code section
# (0x01010018) that Zephyr's board linker script emits is removed, then the Kestrel image gate runs
# on the stripped ELF (nothing below 0x4000, nothing outside code flash).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=$HOME/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
export PATH=$HOME/.local/bin:$PATH ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=$TC
ZB=${ZEPHYR_WS:-$HOME/Projects/_ref/zephyrproject}
(cd "$ZB/zephyr" && west build -b arduino_uno_r4@wifi "$ROOT/baselines/zephyr" -d "$ROOT/build/base_zephyr" >/dev/null)
E=$ROOT/build/base_zephyr/zephyr/zephyr.elf
OUT=$ROOT/build/base_zephyr/image.elf
SECS=$($TC/bin/arm-none-eabi-objdump -h "$E" | awk '$4 ~ /^0101/ {print $2}')
ARGS=(); for s in $SECS; do ARGS+=(-R "$s"); done
$TC/bin/arm-none-eabi-objcopy "${ARGS[@]}" "$E" "$OUT"
python3 "$ROOT/scripts/check_image.py" "$OUT"
$TC/bin/arm-none-eabi-objcopy -O binary "$OUT" "$ROOT/build/base_zephyr/image.bin"
echo "removed sections: $SECS; image $(stat -c %s "$ROOT/build/base_zephyr/image.bin") bytes"
