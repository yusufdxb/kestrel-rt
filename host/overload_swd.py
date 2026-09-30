#!/usr/bin/env python3
"""Overload points beyond the telemetry-starvation boundary, measured through non-halting SWD reads
of the kernel counters (per-step deltas). Work is set through the host link and confirmed by the
counters changing; comms still gets occasional CPU at these loads.
    python3 host/overload_swd.py NAME --scales 5.5 6 6.5 7 8 --secs 10"""
import argparse
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import kestrel_link as kl  # noqa: E402

BASE = {"control": 20, "safety": 10, "sensor": 10, "health": 50}
TID = {"control": 0, "safety": 1, "sensor": 2, "comms": 3, "health": 4, "logger": 5}
NAMES = {v: k for k, v in TID.items()}


def swd_reset():
    from pyocd.core.helpers import ConnectHelper
    from pyocd.core.target import Target
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target
        t.write16(0x4001E3FE, 0xA502)  # PRCR: make sure no bootloader double-tap magic is left
        t.write32(0x4001E500, 0)
        t.write16(0x4001E3FE, 0xA500)
        t.reset(Target.ResetType.SYSRESETREQ)


SYSTEM = "kestrel"


def kstats():
    if SYSTEM == "kestrel":
        out = subprocess.run([sys.executable, str(ROOT / "host/swd_kstats.py"), str(ROOT / "build/benchmark/kestrel.elf")],
                             capture_output=True, text=True, check=True).stdout
        return json.loads(out)
    return bench_stats(ROOT / f"build/base_{SYSTEM}/image.elf")


def bench_stats(elf):
    """Baselines: read bench_stats_t s_st (jobs[8], misses[8], overruns[8], ...) without halting."""
    from elftools.elf.elffile import ELFFile
    from pyocd.core.helpers import ConnectHelper
    with open(elf, "rb") as f:
        ef = ELFFile(f)
        addr = next(sy["st_value"] for sy in ef.get_section_by_name(".symtab").iter_symbols() if sy.name == "s_st")
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target
        jobs = [t.read32(addr + 4 * i) for i in range(6)]
        miss = [t.read32(addr + 32 + 4 * i) for i in range(6)]
    return {"tasks": [{"id": i, "jobs": jobs[i], "releases": jobs[i], "deadline_misses": miss[i], "overruns": 0, "wcet": 0}
                      for i in range(6)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--scales", nargs="+", type=float, required=True)
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--system", default="kestrel")
    a = ap.parse_args()
    global SYSTEM
    SYSTEM = a.system
    steps = []
    for sc in a.scales:
        # Start every step from a warm reset: once overloaded, the (lower-priority) command path
        # starves and the load can no longer be changed from the host.
        swd_reset()
        time.sleep(3.0)
        lk = kl.Link()
        lk.poll(0.5)
        for k, v in BASE.items():
            units = struct.pack("<H", max(1, round(v * sc)))
            for _ in range(4):
                lk.send("set_work", bytes([TID[k]]) + units)
                if lk.wait_for(0x06, 0.5, lambda f: f[2][0] == 0x10):
                    break
        lk.ser.close()
        time.sleep(1.0)
        s0 = kstats()
        time.sleep(a.secs)
        s1 = kstats()
        step = {"scale": sc, "secs": a.secs, "tasks": {}}
        for t0, t1 in zip(s0["tasks"], s1["tasks"]):
            step["tasks"][NAMES.get(t1["id"], str(t1["id"]))] = {
                k: t1[k] - t0[k] for k in ("jobs", "releases", "deadline_misses", "overruns")}
            step["tasks"][NAMES.get(t1["id"], str(t1["id"]))]["wcet_cycles_lifetime_max"] = t1["wcet"]
        steps.append(step)
        print(f"scale {sc:4.2f} " + "  ".join(f"{n}:{v['jobs']}j/{v['deadline_misses']}m/{v['overruns']}o"
                                                for n, v in step["tasks"].items()), flush=True)
    (ROOT / "results" / f"{a.name}.json").write_text(json.dumps({"name": a.name, "system": a.system, "method": "swd-nonhalting", "steps": steps}, indent=1))


if __name__ == "__main__":
    main()
