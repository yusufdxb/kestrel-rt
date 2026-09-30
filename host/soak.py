#!/usr/bin/env python3
"""Long soak: stream only control-task job records (plus status frames) for many hours and keep
exact per-hour and total statistics on the host, without depending on the link staying up:
the authoritative counters (jobs, misses, overruns, faults, resets) are the target's own, read
from status frames; if the bridge drops the link, the soak keeps running on the board and the
host reconnects (it never power-cycles USB on its own).
    python3 host/soak.py NAME --hours 3.0
Outputs: results/raw/NAME.bin (raw stream, gitignored; sha256 in results/MANIFEST.sha256),
results/NAME.json (summary with hourly percentiles of control start latency)."""
import argparse
import hashlib
import json
import math
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BIN_US = 0.5  # histogram resolution for start latency


def pct(hist, total, q):
    target, acc = math.ceil(q / 100 * total), 0
    for b in sorted(hist):
        acc += hist[b]
        if acc >= target:
            return (b + 1) * BIN_US
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--hours", type=float, default=3.0)
    a = ap.parse_args()
    raw = (ROOT / "results/raw" / f"{a.name}.bin").open("wb")
    sha = hashlib.sha256()
    lk = kl.Link()
    lk.poll(0.5)
    for _ in range(4):  # control job records only (12 kB/s), default trace mask
        lk.send("set_trace", struct.pack("<IB", 0xFFFFFFFF & ~sum(1 << b for b in (1, 2, 3, 4, 5, 6, 13, 14, 15)), 0x01))
        if lk.wait_for(0x06, 0.6, lambda f: f[2][0] == 0x04):
            break
    t_start = time.time()
    hourly, cur = [], {"hist": {}, "n": 0, "max": 0.0, "miss_flags": 0}
    total = {"hist": {}, "n": 0, "max": 0.0, "miss_flags": 0}
    first_status = last_status = None
    boots = set()
    last_seq, gaps = None, 0
    hour_end = t_start + 3600
    link_outages = []
    last_rx = time.time()
    while time.time() - t_start < a.hours * 3600:
        frames = lk.poll(0.5)
        chunk = bytes(lk.raw)
        lk.raw = bytearray()
        if chunk:
            raw.write(chunk)
            sha.update(chunk)
            last_rx = time.time()
        elif time.time() - last_rx > 30:
            link_outages.append(round(time.time() - t_start))
            lk.reopen()
            last_rx = time.time()
        for t, s, p in frames:
            if t == 0x02:
                st = kl.parse_status(p)
                boots.add(st["boot_count"])
                if first_status is None:
                    first_status = st
                last_status = st
            elif t == 0x03:
                for task, flags, seq, rl, sl, e16, r16 in kl.parse_jobs(p):
                    if task != 0:
                        continue
                    if last_seq is not None:
                        gaps += (seq - last_seq - 1) & 0xFFFF
                    last_seq = seq
                    us = sl / 48.0
                    b = int(us / BIN_US)
                    for h in (cur, total):
                        h["hist"][b] = h["hist"].get(b, 0) + 1
                        h["n"] += 1
                        h["max"] = max(h["max"], us)
                        h["miss_flags"] += flags & 1
        if time.time() >= hour_end:
            hourly.append({k: cur[k] for k in ("n", "max", "miss_flags")} |
                          {f"p{q}": pct(cur["hist"], cur["n"], q) for q in (50, 99, 99.9)})
            print(json.dumps(hourly[-1]), flush=True)
            cur = {"hist": {}, "n": 0, "max": 0.0, "miss_flags": 0}
            hour_end += 3600
    raw.close()
    summ = {"name": a.name, "hours": a.hours, "raw_sha256": sha.hexdigest(),
            "boot_counts_seen": sorted(boots), "link_outages_s": link_outages,
            "control_records": total["n"], "control_record_gaps": gaps, "control_miss_flags": total["miss_flags"],
            "control_start_us": {f"p{q}": pct(total["hist"], total["n"], q) for q in (50, 95, 99, 99.9, 99.99)} |
            {"max": total["max"]}, "hourly": hourly}
    if first_status and last_status:
        summ["target_counters_delta"] = {
            "uptime_s": last_status["uptime_s"] - first_status["uptime_s"],
            "tasks": [{"id": b["id"], "jobs": b["jobs"] - a_["jobs"], "misses": b["misses"] - a_["misses"],
                       "overruns": b["overruns"] - a_["overruns"], "faults": b["faults"] - a_["faults"]}
                      for a_, b in zip(first_status["tasks"], last_status["tasks"])],
            "wd_warnings": last_status["wd_warnings"] - first_status["wd_warnings"],
            "faults": last_status["faults"] - first_status["faults"]}
        summ["target_totals_at_end"] = {"uptime_s": last_status["uptime_s"], "control_jobs": last_status["tasks"][0]["jobs"],
                                        "control_misses": last_status["tasks"][0]["misses"], "boot_count": last_status["boot_count"]}
    (ROOT / "results" / f"{a.name}.json").write_text(json.dumps(summ, indent=1))
    with open(ROOT / "results/MANIFEST.sha256", "a") as mf:
        mf.write(f"{summ['raw_sha256']}  raw/{a.name}.bin\n")
    print(json.dumps({k: summ[k] for k in ("control_records", "control_record_gaps", "control_miss_flags", "control_start_us", "boot_counts_seen")}))


if __name__ == "__main__":
    main()
