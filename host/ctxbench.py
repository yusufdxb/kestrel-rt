#!/usr/bin/env python3
"""Read the context-switch microbenchmark histograms from a running ctxbench image (Kestrel or
FreeRTOS variant): signal_to_wake and block_to_resume, in timer ticks (48 MHz) -> ns.
    python3 host/ctxbench.py NAME [--secs 20]"""
import argparse
import json
import math
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

NS_PER_TICK = 1e9 / 48e6


def fetch(lk):
    for _ in range(4):
        lk.send("app_stats")
        parts = {}
        end = time.time() + 1.5
        while time.time() < end and len(parts) < 4:
            for t, s, p in lk.poll(0.1):
                if t == 0x08 and len(p) == 276:
                    parts[(p[0], p[1])] = p
        if len(parts) == 4:
            return parts
    raise SystemExit("histograms not received")


def decode(parts, which):
    bins = [0] * 128
    for part in (0, 1):
        p = parts[(which, part)]
        _, _, width = struct.unpack_from("<BBH", p, 0)
        count, mx, mn, ovf = struct.unpack_from("<4I", p, 4)
        bins[part * 64:(part + 1) * 64] = struct.unpack_from("<64I", p, 20)
    out = {"count": count, "min_ns": mn * NS_PER_TICK, "max_ns": mx * NS_PER_TICK, "overflow": ovf,
           "bin_width_ns": width * NS_PER_TICK}
    for q in (50, 95, 99, 99.9):
        target, acc = math.ceil(q / 100 * count), 0
        val = None
        for i, c in enumerate(bins):
            acc += c
            if acc >= target:
                val = (i + 1) * width * NS_PER_TICK  # upper bin edge (conservative)
                break
        out[f"p{q}_ns"] = val if val is not None else out["max_ns"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--secs", type=float, default=20.0)
    a = ap.parse_args()
    lk = kl.Link()
    lk.poll(a.secs)
    parts = fetch(lk)
    res = {"name": a.name, "secs": a.secs, "signal_to_wake": decode(parts, 0), "block_to_resume": decode(parts, 1)}
    Path(__file__).resolve().parents[1].joinpath("results", f"{a.name}.json").write_text(json.dumps(res, indent=1))
    for k in ("signal_to_wake", "block_to_resume"):
        v = res[k]
        print(f"{a.name:22s} {k:16s} n={v['count']:7d} min={v['min_ns']:7.0f} p50={v['p50_ns']:7.0f} "
              f"p99={v['p99_ns']:7.0f} p99.9={v['p99.9_ns']:7.0f} max={v['max_ns']:7.0f} ns ovf={v['overflow']}")


if __name__ == "__main__":
    main()
