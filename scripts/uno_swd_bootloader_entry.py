#!/usr/bin/env python3
# Provenance: copied unchanged (except this line) from ~/Projects/pace-id scripts/uno_swd_bootloader_entry.py
# via ~/Projects/chronos-r4; same author. Proven upload-entry path for THIS board (docs/BOARD_NOTES.md).
"""SWD-assisted Arduino bootloader entry for THIS UNO R4 WiFi (board-specific upload workaround).

Observed on this board only (not reproduced on another board): the ESP32 bridge's automatic
1200-baud double-reset does not leave the RA4M1 in the Arduino bootloader. This helper performs
the bootloader's own double-tap handshake directly:

  1. read SYSTEM.VBTBKR[0..3] (0x4001E500, the bootloader's BOOT_DOUBLE_TAP_DATA on Cortex-M4 parts)
  2. write the double-tap magic 0x07738135 there (PRCR.PRC1 is unlocked only if the plain write does
     not stick, exactly as the bootloader does, and PRCR is restored afterwards)
  3. read back and verify
  4. SYSRESETREQ (MD/BOOT must already be high; asserted: core not in 0x0100xxxx ROM)
  5. sample PC: must be < 0x4000 (bootloader), repeatedly over ~1 s (stays in bootloader)
Nothing else is written. Flash is never touched. Prints a JSON record; exit 0 only if the
bootloader is confirmed resident.
"""
import json
import sys
import time

from pyocd.core.helpers import ConnectHelper
from pyocd.core.target import Target

VBTBKR0 = 0x4001E500
PRCR = 0x4001E3FE
MAGIC = 0x07738135
BOOT_END = 0x4000


def main():
    rec = {}
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target
        t.halt()
        pc0 = t.read_core_register("pc")
        rec["pc_before"] = hex(pc0)
        if 0x01000000 <= pc0 < 0x01010000:
            rec["error"] = "core in factory boot ROM (MD low); set 1200 baud first"
            print(json.dumps(rec)); return 2
        rec["vbtbkr0_before"] = hex(t.read32(VBTBKR0))
        prcr0 = t.read16(PRCR)
        rec["prcr_before"] = hex(prcr0)
        t.write32(VBTBKR0, MAGIC); t.flush()
        v = t.read32(VBTBKR0)
        rec["plain_write_readback"] = hex(v)
        rec["prcr_unlock_used"] = False
        if v != MAGIC:
            rec["prcr_unlock_used"] = True
            t.write16(PRCR, 0xA502)            # PRKEY 0xA5, PRC1 = 1 (same as BSP_PRV_PRCR_PRC1_UNLOCK)
            t.write32(VBTBKR0, MAGIC)
            t.write16(PRCR, 0xA500 | (prcr0 & 0x000B))  # restore previous protect bits
            t.flush()
            v = t.read32(VBTBKR0)
        rec["vbtbkr0_written"] = hex(v)
        rec["prcr_after"] = hex(t.read16(PRCR))
        if v != MAGIC:
            rec["error"] = "magic did not stick"
            print(json.dumps(rec)); return 3
        t.reset(Target.ResetType.SYSRESETREQ)  # core runs; bootloader sees magic, clears it, stays
        samples = []
        for _ in range(5):
            time.sleep(0.2)
            t.halt(); samples.append(t.read_core_register("pc")); t.resume()
        rec["pc_samples_after_reset"] = [hex(p) for p in samples]
        rec["vbtbkr0_after_reset"] = hex(t.read32(VBTBKR0))
        rec["in_bootloader"] = all(p < BOOT_END for p in samples)
    print(json.dumps(rec))
    return 0 if rec["in_bootloader"] else 4


if __name__ == "__main__":
    sys.exit(main())
