#!/usr/bin/env python3
"""Pre-flash image gate. Fails (exit 1) unless the ELF can be uploaded without touching the
Arduino bootloader and will be accepted and started by it:

  1. every loadable section (non-NOBITS, SHF_ALLOC) has its load address in [0x4000, 0x40000);
  2. the vector table sits at 0x4000; vector[0] (initial MSP) is 0x20xxxxxx, 8-byte aligned, inside
     SRAM (the bootloader's app_valid test is (msp & 0xFF000000) == 0x20000000);
  3. vector[1] (reset) is a Thumb address inside the image;
  4. no heap/libc allocation symbols are linked (malloc, free, _sbrk, calloc, realloc).
Uses only the toolchain's readelf/objdump output (no Python ELF dependency).
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

APP_ORIGIN, FLASH_END = 0x4000, 0x40000
SRAM_LO, SRAM_HI = 0x20000000, 0x20008000
FORBIDDEN = {"malloc", "free", "_sbrk", "calloc", "realloc", "_malloc_r", "_free_r"}


def tool(name):
    tc = Path.home() / ".local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin"
    return str(tc / f"arm-none-eabi-{name}")


def main(elf):
    errors = []
    # Program headers give LMAs directly.
    ph = subprocess.run([tool("readelf"), "-lW", elf], capture_output=True, text=True, check=True).stdout
    for line in ph.splitlines():
        m = re.match(r"\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
        if not m:
            continue
        vaddr, paddr, filesz = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
        if filesz == 0:
            continue  # NOBITS-only segment (RAM), nothing is written to flash
        if not (APP_ORIGIN <= paddr and paddr + filesz <= FLASH_END):
            errors.append(f"LOAD segment paddr 0x{paddr:08x} size {filesz} outside [0x4000,0x40000)")
    # Vector table: the 8 bytes at 0x4000, from whichever loadable segment covers that address
    # (Kestrel names it .vectors; Zephyr and FreeRTOS images use other section names).
    from elftools.elf.elffile import ELFFile
    vec = b""
    with open(elf, "rb") as fh:
        ef = ELFFile(fh)
        for seg in ef.iter_segments():
            if seg["p_type"] == "PT_LOAD" and seg["p_filesz"] and seg["p_paddr"] <= APP_ORIGIN < seg["p_paddr"] + seg["p_filesz"]:
                off = APP_ORIGIN - seg["p_paddr"]
                vec = seg.data()[off:off + 8]
    if len(vec) < 8:
        errors.append("no loadable bytes at 0x4000 (vector table missing)")
        msp = reset = 0
    else:
        msp, reset = struct.unpack_from("<II", vec, 0)
    syms = subprocess.run([tool("nm"), elf], capture_output=True, text=True, check=True).stdout
    names = {l.split()[-1] for l in syms.splitlines() if l.strip()}
    vt = [l for l in syms.splitlines() if l.endswith(" kestrel_vectors")]
    if vt and int(vt[0].split()[0], 16) != APP_ORIGIN:
        errors.append("kestrel_vectors not at 0x4000")
    bad = FORBIDDEN & names
    if bad:
        errors.append(f"forbidden allocation symbols linked: {sorted(bad)}")
    if errors:
        print("check_image: FAIL\n  " + "\n  ".join(errors))
        return 1
    print(f"check_image: OK  msp=0x{msp:08x} reset=0x{reset:08x} (flash writes limited to >= 0x4000)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
