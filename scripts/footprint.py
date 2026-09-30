#!/usr/bin/env python3
"""Attribute flash/RAM bytes of an image to components by parsing the GNU ld map file.
    python3 scripts/footprint.py MAPFILE [--json OUT]
Flash = .text + .rodata + initialized-data load image; RAM = .data + .bss + stacks/noinit.
Components are derived from the input object path (kernel/, drivers/, apps/, FreeRTOS, zephyr...)."""
import json
import re
import sys
from collections import defaultdict

FLASH_SECS = ("text", "rodata", "vectors", "exidx", "init_array", "rom_start", "ctors", "device_", "log_const",
              "sw_isr_table", "initlevel", "_static_thread_data", "irq_info")
RAM_SECS = ("data", "bss", "noinit", "task_stacks", "app_shared", "stack")


def component(path):
    p = path.replace("\\", "/")
    for key, name in (("FreeRTOS-Kernel", "freertos-kernel"), ("/zephyr/kernel", "zephyr-kernel"),
                      ("/zephyr/arch", "zephyr-arch"), ("/zephyr/drivers", "zephyr-drivers"),
                      ("/zephyr/soc", "zephyr-soc"), ("/zephyr/lib", "zephyr-lib"), ("hal_renesas", "zephyr-hal"),
                      ("/zephyr/", "zephyr-other"), ("libgcc", "libgcc"), ("libc", "libc"),
                      ("/kernel/", "kestrel-kernel"), ("/arch/", "kestrel-kernel"), ("/safety/", "kestrel-kernel"),
                      ("/trace/", "trace+telemetry"), ("/startup/", "startup"), ("/drivers/", "drivers"),
                      ("/baselines/common", "bench-shim"), ("/baselines/", "baseline-app"), ("/apps/", "app")):
        if key in "/" + p:
            return name
    return "other"


def parse(mapfile):
    """Walk the map: output sections start in column 0; input-section lines carry address, size and
    the contributing object. Only allocated output sections count (debug/comment/attributes do not)."""
    flash, ram = defaultdict(int), defaultdict(int)
    outsec = ""
    pending = None
    started = False
    for raw in open(mapfile, errors="replace"):
        line = raw.rstrip("\n")
        if line.startswith("Linker script and memory map"):
            started = True
            continue
        if not started or not line.strip():
            continue
        if not line[0].isspace():
            outsec = line.split()[0]
            continue
        if outsec.startswith((".debug", ".comment", ".ARM.attributes", "/DISCARD/", ".stab")):
            continue
        parts = line.split()
        # input section on one line: " .text.foo 0xADDR 0xSIZE obj"; or name alone, values on the next line
        if len(parts) == 1 and parts[0].startswith("."):
            pending = parts[0]
            continue
        vals = parts if parts[0].startswith("0x") else parts[1:]
        if len(vals) < 3 or not vals[0].startswith("0x") or not vals[1].startswith("0x"):
            pending = None
            continue
        addr, size, obj = int(vals[0], 16), int(vals[1], 16), vals[2]
        pending = None
        if size == 0 or not (obj.endswith((".o", ".obj")) or ".a(" in obj):
            continue
        comp = component(obj)
        if 0x20000000 <= addr < 0x20008000:
            ram[comp] += size
            if outsec.lstrip(".").startswith(("data", "app_shared", "datas", "device_states")):
                flash[comp] += size  # initial values live in flash too
        elif 0x4000 <= addr < 0x40000:
            flash[comp] += size
    return flash, ram


if __name__ == "__main__":
    f, r = parse(sys.argv[1])
    out = {"flash": dict(sorted(f.items())), "ram": dict(sorted(r.items())), "flash_total": sum(f.values()),
           "ram_total": sum(r.values())}
    print(json.dumps(out, indent=1))
    if "--json" in sys.argv:
        open(sys.argv[sys.argv.index("--json") + 1], "w").write(json.dumps(out, indent=1))
