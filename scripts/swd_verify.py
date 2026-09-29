#!/usr/bin/env python3
"""READ-ONLY SWD checks (never writes flash, never halts unless asked):
  boot   : bootloader block 0x0000-0x3FFF equals the official dfu_wifi.hex image (0xFF elsewhere)
  app BIN: flash from 0x4000 equals BIN byte for byte
Exit 0 only if every requested check passes. Prints a JSON line."""
import hashlib
import json
import sys
from pathlib import Path

from pyocd.core.helpers import ConnectHelper

HEX = Path.home() / ".arduino15/packages/arduino/hardware/renesas_uno/1.6.0/bootloaders/UNO_R4/dfu_wifi.hex"
HEX_SHA = "c507c084e4a1dc6501d1f8dab3c33b687e7edab402ba20c6854f50c28529f612"


def hex_image(end=0x4000):
    img, base = {}, 0
    for line in open(HEX):
        n, a, ty = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        if ty == 4:
            base = int(line[9:13], 16) << 16
        elif ty == 0:
            for i in range(n):
                img[base + a + i] = int(line[9 + 2 * i:11 + 2 * i], 16)
    return bytes(img.get(a, 0xFF) for a in range(end))


def main(argv):
    rec = {"ok": True}
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target
        if "boot" in argv:
            assert hashlib.sha256(HEX.read_bytes()).hexdigest() == HEX_SHA, "dfu_wifi.hex changed"
            want = hex_image()
            got = bytes(t.read_memory_block8(0, 0x4000))
            bad = sum(1 for a, b in zip(want, got) if a != b)
            rec["bootloader_sha256"] = hashlib.sha256(got).hexdigest()
            rec["bootloader_mismatches"] = bad
            rec["ok"] &= bad == 0
        if "app" in argv:
            binp = Path(argv[argv.index("app") + 1])
            want = binp.read_bytes()
            got = bytes(t.read_memory_block8(0x4000, len(want)))
            bad = sum(1 for a, b in zip(want, got) if a != b)
            rec["app_bytes"] = len(want)
            rec["app_mismatches"] = bad
            rec["ok"] &= bad == 0
    print(json.dumps(rec))
    return 0 if rec["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
