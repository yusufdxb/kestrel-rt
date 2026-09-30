#!/usr/bin/env python3
"""Watchdog policy for NONcritical tasks: a hang must set the degraded flag and must NOT reset.
For each target (comms, health, logger): inject a hang, wait, read kernel state over SWD without
halting (comms is the telemetry path, so the link is unusable once it hangs), then SWD-reset."""
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import kestrel_link as kl  # noqa: E402
from overload_swd import swd_reset  # noqa: E402

ELF = ROOT / "build/benchmark/kestrel.elf"
T = {"comms": 3, "health": 4, "logger": 5}


def read_kernel():
    from elftools.elf.elffile import ELFFile
    from pyocd.core.helpers import ConnectHelper
    import swd_kstats as sk
    with open(ELF, "rb") as f:
        ef = ELFFile(f)
        kern = sk.struct_layout(ef.get_dwarf_info(), "kernel_t")
        sym = {s.name: s["st_value"] for s in ef.get_section_by_name(".symtab").iter_symbols()}
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        t = s.target
        return {"degraded": t.read32(sym["g_k"] + kern["degraded"])}


def main():
    results = []
    for name, tid in T.items():
        swd_reset()
        time.sleep(3.0)
        lk = kl.Link()
        lk.poll(0.5)
        st0 = None
        for f in lk.poll(1.5):
            if f[0] == 0x02:
                st0 = kl.parse_status(f[2])
        for _ in range(4):
            lk.send("inject", bytes([9, tid]) + b"\0\0")
            if lk.wait_for(0x06, 0.6, lambda fr: fr[2][0] == 0x20):
                break
        lk.ser.close()
        k0 = json.loads(subprocess.run([sys.executable, str(ROOT / "host/swd_kstats.py"), str(ELF)], capture_output=True, text=True, check=True).stdout)
        time.sleep(8.0)
        k1 = json.loads(subprocess.run([sys.executable, str(ROOT / "host/swd_kstats.py"), str(ELF)], capture_output=True, text=True, check=True).stdout)
        kd = read_kernel()
        crit = {i: {"jobs": k1["tasks"][i]["jobs"] - k0["tasks"][i]["jobs"],
                    "misses": k1["tasks"][i]["deadline_misses"] - k0["tasks"][i]["deadline_misses"]} for i in range(3)}
        # a reset would restart the counters: jobs must have kept increasing monotonically
        no_reset = all(k1["tasks"][i]["jobs"] > k0["tasks"][i]["jobs"] for i in range(3)) and \
            k1["alarm_count"] > k0["alarm_count"]
        ok = no_reset and (kd["degraded"] >> tid) & 1 == 1 and all(v["misses"] == 0 for v in crit.values())
        r = {"hung_task": name, "degraded_mask": hex(kd["degraded"]), "no_reset": no_reset, "critical_tasks_8s": crit,
             "boot_count_before": st0["boot_count"] if st0 else None, "verdict": "PASS" if ok else "FAIL"}
        print(json.dumps(r), flush=True)
        results.append(r)
    swd_reset()
    (ROOT / "results/wd_degraded.json").write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
