#!/usr/bin/env python3
"""Arm the flight recorder with a record mask and a trigger mask, collect the frozen window, print
it relative to the first matching anchor event.
    python3 host/timeline.py RECORD_TYPES TRIGGER_TYPES [--post N] [--lines 0,11] [--out FILE]
Types are comma-separated numbers from trace/trace.h (1 RELEASE, 2 START, 4 SWITCH, 5 ISR_ENTER,
6 ISR_EXIT, 7 DEADLINE_MISS, 11 FAULT, 13 BLOCK, 16 USER, ...)."""
import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

NAMES = {0: "control", 1: "safety", 2: "sensor", 3: "comms", 4: "health", 5: "logger", 8: "idle", 255: "kernel"}
LINES = {0: "TIMER_CMP", 1: "TIMER_OVF", 2: "UART_RXI", 3: "UART_TEI", 4: "UART_ERI", 5: "DMA_TX", 6: "ADC",
         7: "STORM", 11: "ADC_TRIG"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("record")
    ap.add_argument("trigger")
    ap.add_argument("--post", type=int, default=100)
    ap.add_argument("--lines", default=None, help="keep ISR events only for these ICU lines")
    ap.add_argument("--secs", type=float, default=6.0)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    rec = sum(1 << int(x) for x in a.record.split(","))
    trig = sum(1 << int(x) for x in a.trigger.split(","))
    lk = kl.Link()
    lk.poll(0.3)
    lk.send("trace_stream")
    lk.poll(0.3)
    for _ in range(4):
        lk.send("trace_arm", struct.pack("<IIH", rec, trig, a.post))
        if lk.wait_for(0x06, 0.8, lambda f: f[2][0] == 0x0B):
            break
    else:
        raise SystemExit("arm not acknowledged")
    ev, end = [], time.time() + a.secs
    while time.time() < end:
        for t, s, p in lk.poll(0.1):
            if t == 0x04:
                ev += kl.parse_trace(p)
    lk.send("trace_stream")
    lk.poll(0.3)
    keep = {int(x) for x in a.lines.split(",")} if a.lines else None
    if keep is not None:
        ev = [e for e in ev if e[1] not in (5, 6) or e[2] in keep]
    if not ev:
        raise SystemExit("no events")
    base = ev[0][0]
    rows = []
    for ts, ty, i, d in ev:
        rel = (((ts - base) + 2**31) % 2**32) - 2**31
        name = LINES.get(i, i) if ty in (5, 6) else NAMES.get(i, i)
        rows.append((round(rel / 48, 2), kl.TE.get(ty, ty), name, d))
        print(f"{rel / 48:10.2f} us {kl.TE.get(ty, ty):14s} {name!s:10s} {d}")
    if a.out:
        Path(a.out).write_text(json.dumps(rows))


if __name__ == "__main__":
    main()
