#!/usr/bin/env python3
"""Read Kestrel kernel counters over SWD WITHOUT halting the core (a halt would stall the tasks
while the timebase keeps running and trip the watchdog supervisor). Field offsets come from the
ELF's DWARF info, so the reader follows the struct layout of the flashed build.
    python3 host/swd_kstats.py build/benchmark/kestrel.elf"""
import json
import sys

from elftools.elf.elffile import ELFFile
from pyocd.core.helpers import ConnectHelper

FIELDS = ["jobs", "releases", "deadline_misses", "overruns", "wcet", "max_response", "faults", "state"]


def struct_layout(dwarf, name):
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag == "DW_TAG_typedef" and die.attributes.get("DW_AT_name", None) and \
                    die.attributes["DW_AT_name"].value == name.encode():
                st = die.get_DIE_from_attribute("DW_AT_type")
                out = {"__size": st.attributes["DW_AT_byte_size"].value}
                for m in st.iter_children():
                    if m.tag == "DW_TAG_member" and "DW_AT_data_member_location" in m.attributes:
                        out[m.attributes["DW_AT_name"].value.decode()] = m.attributes["DW_AT_data_member_location"].value
                return out
    raise SystemExit(f"type {name} not found")


def main(elf):
    with open(elf, "rb") as f:
        ef = ELFFile(f)
        dw = ef.get_dwarf_info()
        tcb = struct_layout(dw, "ktcb_t")
        kern = struct_layout(dw, "kernel_t")
        sym = {s.name: s["st_value"] for s in ef.get_section_by_name(".symtab").iter_symbols()}
    base = sym["g_k"]
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target  # memory reads only; never halt
        ntasks = t.read32(base + kern["ntasks"])
        rows = []
        for i in range(ntasks):
            a = base + kern["tcb"] + i * tcb["__size"]
            r = {"id": i}
            for fld in FIELDS:
                r[fld] = t.read8(a + tcb[fld]) if fld == "state" else t.read32(a + tcb[fld])
            rows.append(r)
        out = {"alarm_count": t.read32(base + kern["alarm_count"]), "context_switches": t.read32(base + kern["context_switches"]),
               "tasks": rows}
    print(json.dumps(out))


if __name__ == "__main__":
    main(sys.argv[1])
