#!/usr/bin/env python3
"""Fault-injection campaign on the physical board (docs/TEST_PLAN.md, section F).

For each case: clear the stored crash record, inject at the start of the target task's next job,
then observe. Two expected outcomes:
  contain : no reset (boot_count unchanged), target task FAULTED, control keeps meeting deadlines,
            crash record present with the expected fault class.
  reset   : boot_count increments, crash record retrieved after reboot with the expected class,
            reset cause matches (software reset for faults, WDT for watchdog cases).
Every verdict is computed from target-reported data. Results -> results/fault_campaign.json.
"""
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ELF = ROOT / "build/benchmark/kestrel.elf"
T = {"control": 0, "safety": 1, "sensor": 2, "comms": 3, "health": 4, "logger": 5}
CFSR = {0: "IACCVIOL", 1: "DACCVIOL", 3: "MUNSTKERR", 4: "MSTKERR", 5: "MLSPERR", 7: "MMARVALID",
        8: "IBUSERR", 9: "PRECISERR", 10: "IMPRECISERR", 11: "UNSTKERR", 12: "STKERR", 13: "LSPERR",
        15: "BFARVALID", 16: "UNDEFINSTR", 17: "INVSTATE", 18: "INVPC", 19: "NOCP", 24: "UNALIGNED",
        25: "DIVBYZERO"}

# (name, inject kind, target task, expected outcome, expected crash kind, expected CFSR bits, arg)
CASES = [
    ("null_read_contained", 1, "health", "contain", 2, ["DACCVIOL", "MMARVALID"], 0),
    ("null_write_contained", 2, "logger", "contain", 2, ["DACCVIOL", "MMARVALID"], 0),
    ("illegal_insn_contained", 3, "health", "contain", 4, ["UNDEFINSTR"], 0),
    ("div_zero_contained", 4, "logger", "contain", 4, ["DIVBYZERO"], 0),
    ("unmapped_read_unpriv", 5, "health", "contain", 2, ["DACCVIOL", "MMARVALID"], 0),
    ("kernel_write_blocked", 6, "logger", "contain", 2, ["DACCVIOL", "MMARVALID"], 0),
    ("stack_overflow_guard", 7, "health", "contain", 2, [], 0),
    ("exec_from_ram_xn", 10, "logger", "contain", 2, ["IACCVIOL"], 0),
    ("assert_contained", 8, "health", "contain", 6, [], 0),
    ("task_return_contained", 11, "logger", "contain", 7, [], 0),
    ("null_read_critical_reset", 1, "control", "reset", 2, ["DACCVIOL", "MMARVALID"], 0),
    ("div_zero_critical_reset", 4, "sensor", "reset", 4, ["DIVBYZERO"], 0),
    ("busfault_privileged_reset", 5, "comms", "reset", 3, ["PRECISERR", "BFARVALID"], 0),
    ("stack_overflow_critical_reset", 7, "safety", "reset", 2, [], 0),
    ("assert_critical_reset", 8, "control", "reset", 6, [], 0),
    ("hang_control_watchdog", 9, "control", "reset", 8, [], 0),
    ("hang_sensor_watchdog", 9, "sensor", "reset", 8, [], 0),
]


def symbols():
    out = subprocess.run([str(Path.home() / ".local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm"),
                          "-S", str(ELF)], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4:
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    return syms


def func_at(syms, pc):
    for a, sz, n in syms:
        if a <= (pc & ~1) < a + sz:
            return n
    return "?"


def latest_status(lk, secs=2.5):
    st = None
    end = time.time() + secs
    while time.time() < end:
        for t, s, p in lk.poll(0.1):
            if t == 0x02:
                st = kl.parse_status(p)
        if st:
            return st
    return st


def get_crash(lk):
    for _ in range(4):
        lk.send("get_crash")
        f = lk.wait_for(0x05, 1.0)
        if f:
            return kl.parse_crash(f[2])
    return None


def cfsr_bits(v):
    return [n for b, n in CFSR.items() if v >> b & 1]


def run_case(lk, syms, case):
    name, kind, task, expect, ckind, cbits, arg = case
    st0 = latest_status(lk)
    if st0 is None:
        return {"name": name, "verdict": "FAIL", "why": "no status before injection"}
    lk.send("clear_crash")
    lk.poll(0.3)
    t_inj = time.time()
    lk.send("inject", bytes([kind, T[task]]) + struct.pack("<H", arg))
    if expect == "contain":
        # host->target frames can be lost in the bridge: require the ACK (the board keeps running)
        for _attempt in range(3):
            if lk.wait_for(0x06, 0.6, lambda f: f[2][0] == 0x20):
                break
            t_inj = time.time()
            lk.send("inject", bytes([kind, T[task]]) + struct.pack("<H", arg))
        else:
            return {"name": name, "expect": expect, "verdict": "FAIL", "why": "inject never acknowledged"}
    res = {"name": name, "kind": kind, "task": task, "expect": expect, "boot_before": st0["boot_count"]}
    if expect == "contain":
        time.sleep(1.5)
        st1 = latest_status(lk, 3.0)
        crash = get_crash(lk)
        tk = next(x for x in st1["tasks"] if x["id"] == T[task])
        ctl0 = next(x for x in st0["tasks"] if x["id"] == 0)
        ctl1 = next(x for x in st1["tasks"] if x["id"] == 0)
        res.update(boot_after=st1["boot_count"], task_state=tk["state"], task_faults=tk["faults"],
                   control_jobs_delta=ctl1["jobs"] - ctl0["jobs"], control_misses=ctl1["misses"] - ctl0["misses"])
        ok = st1["boot_count"] == st0["boot_count"] and tk["state"] == 3 and ctl1["misses"] == ctl0["misses"] \
            and ctl1["jobs"] > ctl0["jobs"]
    else:
        # wait for the reboot: boot_count increments
        # the reboot itself confirms delivery (an ACK would die in the TX ring at reset)
        st1, t_back = None, None
        seen, acks = [], []
        for _attempt in range(2):
            end = time.time() + 4.0
            while time.time() < end:
                s = None
                for t, sq, p in lk.poll(0.25):
                    if t == 0x02:
                        s = kl.parse_status(p)
                        seen.append({"t": round(time.time() - t_inj, 2), "boot": s["boot_count"],
                                     "up": round(s["uptime_s"], 2), "faults": s["faults"],
                                     "states": [x["state"] for x in s["tasks"]],
                                     "jobs0": s["tasks"][0]["jobs"]})
                    elif t == 0x06:
                        acks.append((round(time.time() - t_inj, 2), p.hex()))
                if s and s["boot_count"] != st0["boot_count"]:
                    st1, t_back = s, time.time()
                    break
            if st1 is not None:
                break
            t_inj = time.time()
            lk.send("inject", bytes([kind, T[task]]) + struct.pack("<H", arg))
        if st1 is None:
            res.update(verdict="FAIL", why="no reboot observed", seen=seen[-6:], acks=acks)
            return res
        crash = get_crash(lk)
        res.update(boot_after=st1["boot_count"], reset_cause=kl.reset_str(st1["reset_cause"]),
                   host_observed_recovery_s=round(t_back - t_inj, 3), uptime_at_first_status_s=round(st1["uptime_s"], 3))
        ok = True
        if ckind == 8:
            ok &= bool(st1["reset_cause"] & 0x2)  # WDT reset
    rec = crash["record"] if crash and crash["present"] else None
    if rec is None:
        res.update(verdict="FAIL", why="no crash record")
        return res
    bits = cfsr_bits(rec["cfsr"])
    res.update(crash_kind=kl.FAULT.get(rec["kind"], rec["kind"]), crash_task=rec["task"], cfsr=hex(rec["cfsr"]),
               cfsr_bits=bits, hfsr=hex(rec["hfsr"]), mmfar=hex(rec["mmfar"]), bfar=hex(rec["bfar"]),
               pc=hex(rec["pc"]), pc_func=func_at(syms, rec["pc"]), lr=hex(rec["lr"]), lr_func=func_at(syms, rec["lr"]),
               sp=hex(rec["sp"]), detail=hex(rec["detail"]), stack_used=rec["stack_used"],
               capture_cycles=rec["capture_cycles"], record_flags=hex(rec["flags"]))
    ok &= rec["kind"] == ckind and all(b in bits for b in cbits)
    if ckind != 8:
        ok &= rec["task"] == T[task]
    res["verdict"] = "PASS" if ok else "FAIL"
    return res


def main():
    only = sys.argv[1:]
    syms = symbols()
    lk = kl.Link()
    lk.poll(0.5)
    results = []
    for case in CASES:
        if only and case[0] not in only:
            continue
        r = run_case(lk, syms, case)
        results.append(r)
        print(json.dumps(r))
        if r["expect"] == "contain":
            # faulted tasks stay parked until reset: reboot to a clean state for the next case
            lk.send("reset")
            time.sleep(2.0)
            lk.poll(0.5)
    out = ROOT / "results" / "fault_campaign.json"
    prev = json.loads(out.read_text()) if out.exists() and only else []
    names = {r["name"] for r in results}
    merged = [p for p in prev if p["name"] not in names] + results
    out.write_text(json.dumps(merged, indent=1))
    print("PASS", sum(r["verdict"] == "PASS" for r in merged), "of", len(merged))


if __name__ == "__main__":
    main()
