#!/usr/bin/env python3
"""Arm the flight recorder, inject one fault, print the frozen event window around it.
Usage: capture_fault.py KIND TASK [POST] [--out results/NAME.json]"""
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

NAMES = {0: "control", 1: "safety", 2: "sensor", 3: "comms", 4: "health", 5: "logger", 8: "idle", 255: "kernel"}
TASKS = {v: k for k, v in NAMES.items()}


def main():
    kind, task = int(sys.argv[1]), TASKS[sys.argv[2]]
    post = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 40
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    lk = kl.Link()
    lk.poll(0.3)
    rec_mask = 0xFFFFFFFF & ~((1 << 5) | (1 << 6))
    trig = (1 << 11) | (1 << 7)  # FAULT, DEADLINE_MISS
    for _ in range(3):
        lk.send("trace_arm", struct.pack("<IIH", rec_mask, trig, post))
        if lk.wait_for(0x06, 0.6, lambda f: f[2][0] == 0x0B):
            break
    lk.poll(0.3)
    for _ in range(3):
        lk.send("inject", bytes([kind, task]) + b"\0\0")
        if lk.wait_for(0x06, 0.6, lambda f: f[2][0] == 0x20):
            break
    ev = []
    end = time.time() + 3.0
    while time.time() < end:
        for t, s, p in lk.poll(0.1):
            if t == 0x04:
                ev += kl.parse_trace(p)
    lk.send("trace_stream")
    lk.poll(0.3)
    if not ev:
        print("no events captured")
        return
    trig_i = next((i for i, e in enumerate(ev) if e[1] in (7, 11)), 0)
    t0 = ev[trig_i][0]
    rows = []
    for ts, typ, i, d in ev:
        rows.append(((ts - t0) & 0xFFFFFFFF if ts >= t0 else -((t0 - ts) & 0xFFFFFFFF), kl.TE.get(typ, typ), NAMES.get(i, i), d))
        print(f"{rows[-1][0] / 48:10.2f} us  {rows[-1][1]:14s} {rows[-1][2]:8s} data={d}")
    if out:
        Path(out).write_text(json.dumps({"kind": kind, "task": sys.argv[2], "events": rows}, indent=0))


if __name__ == "__main__":
    main()
