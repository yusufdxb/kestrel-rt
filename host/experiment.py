#!/usr/bin/env python3
"""Run one measured experiment against the board and summarize it.

    python3 host/experiment.py NAME --secs 30 [--work control=20 sensor=10 ...]
                               [--inject KIND:TASK[:ARG]] [--stall-ms N] [--storm HZ]

Writes results/raw/NAME.bin (raw byte stream, gitignored), results/NAME.json (summary) and appends
the raw file's sha256 to results/MANIFEST.sha256. All timing comes from the target (timer ticks
at 48 MHz, DWT cycles at 48 MHz); the host clock is only used to bound the capture length.
"""
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
TASK_IDS = {"control": 0, "safety": 1, "sensor": 2, "comms": 3, "health": 4, "logger": 5}
HIST_NAMES = {0: "pendsv_cycles", 1: "alarm_isr_cycles", 2: "svc_cycles", 3: "irq_latency_ticks"}
US_PER_TICK = 1e6 / 48e6
# firmware default (trace/trace.c): everything except per-job/per-switch high-rate events
DEFAULT_TRACE_MASK = 0xFFFFFFFF & ~sum(1 << b for b in (1, 2, 3, 4, 5, 6, 13, 14, 15))


def pct(sorted_vals, q):
    if not sorted_vals:
        return None
    k = min(len(sorted_vals) - 1, max(0, math.ceil(q / 100.0 * len(sorted_vals)) - 1))
    return sorted_vals[k]


def summarize(vals, scale=1.0):
    v = sorted(vals)
    if not v:
        return {"n": 0}
    return {"n": len(v), "p50": pct(v, 50) * scale, "p95": pct(v, 95) * scale, "p99": pct(v, 99) * scale,
            "p99.9": pct(v, 99.9) * scale, "max": v[-1] * scale, "min": v[0] * scale}


def hist_percentiles(h):
    """Percentiles from a fixed-width histogram: value = upper edge of the bin (conservative)."""
    total = h["count"]
    out = {"count": total, "max": h["max"], "overflow": h["overflow"], "width": h["width"]}
    if total == 0:
        return out
    for q in (50, 95, 99, 99.9):
        target = math.ceil(q / 100.0 * total)
        acc = 0
        val = None
        for i, c in enumerate(h["bins"]):
            acc += c
            if acc >= target:
                val = (i + 1) * h["width"]
                break
        out[f"p{q}"] = val if val is not None else h["max"]
    return out


def parse_hist_parts(parts):
    h = {"bins": [0] * 128}
    for p in parts:
        which, part, width = struct.unpack_from("<BBH", p, 0)
        count, mx, ovf = struct.unpack_from("<III", p, 4)
        bins = struct.unpack_from("<32I", p, 16)
        h.update(width=width, count=count, max=mx, overflow=ovf)
        h["bins"][part * 32:(part + 1) * 32] = bins
    return h


def parse_bstatus(p):
    now, hi, dropped, irqmax = struct.unpack_from("<4I", p, 0)
    tasks = []
    for i in range(6):
        j, m, w, ms, rlo, rhi = struct.unpack_from("<6I", p, 16 + 24 * i)
        tasks.append({"jobs": j, "misses": m, "wcet_cyc": w, "max_start_ticks": ms, "run_cyc": rlo | rhi << 32})
    return {"uptime_ticks": hi << 32 | now, "jobs_dropped": dropped, "irq_lat_max_ticks": irqmax, "tasks": tasks,
            "name": p[16 + 144:].split(b"\0")[0].decode(errors="replace")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--secs", type=float, default=30.0)
    ap.add_argument("--work", nargs="*", default=[])
    ap.add_argument("--inject", default=None)
    ap.add_argument("--stall-ms", type=int, default=0)
    ap.add_argument("--stall-at", type=float, default=5.0)
    ap.add_argument("--storm", type=int, default=None)
    ap.add_argument("--trace-mask", type=lambda x: int(x, 0), default=None)
    ap.add_argument("--no-reset-stats", action="store_true")
    ap.add_argument("--warmup", type=float, default=1.5)
    a = ap.parse_args()

    lk = kl.Link()
    lk.poll(0.3)
    for w in a.work:
        task, units = w.split("=")
        lk.send("set_work", bytes([TASK_IDS[task]]) + struct.pack("<H", int(units)))
    if a.storm is not None:
        lk.send("storm", struct.pack("<I", a.storm))
    if a.trace_mask is not None:
        lk.send("set_trace", struct.pack("<IB", a.trace_mask, 0xFF))
    lk.poll(a.warmup)  # let new work settle before clearing maxima
    if not a.no_reset_stats:
        lk.send("reset_stats")
    lk.dec = kl.Decoder()
    lk.raw = bytearray()
    t_start = time.time()
    injected = stalled = False
    frames = []
    while time.time() - t_start < a.secs:
        el = time.time() - t_start
        if a.inject and not injected and el > 2.0:
            kind, task, *arg = a.inject.split(":")
            argv = int(arg[0]) if arg else 0
            lk.send("inject", bytes([int(kind), TASK_IDS.get(task, int(task) if task.isdigit() else 0)]) +
                    struct.pack("<H", argv))
            injected = True
        if a.stall_ms and not stalled and el > a.stall_at:
            lk.send("tx_stall", struct.pack("<I", a.stall_ms))
            stalled = True
        frames += lk.poll(0.2)
    # end-of-run: final status, then pause the streams so result frames cannot be dropped.
    frames += lk.poll(1.2)
    n_capture = len(frames)
    lk.send("set_trace", struct.pack("<IB", 0, 0))
    lk.poll(0.6)  # tail of the job stream: excluded from statistics (gap would look like loss)
    for which in HIST_NAMES:
        for _attempt in range(3):
            lk.send("get_hist", bytes([which]))
            got = [f for f in lk.poll(0.6) if f[0] == 0x08 and len(f[2]) == 144 and f[2][0] == which]
            if len({f[2][1] for f in got}) == 4:
                frames += got
                break
    for _attempt in range(3):
        lk.send("app_stats")
        got = [f for f in lk.poll(0.5) if f[0] == 0x08 and len(f[2]) == 40]
        if got:
            frames += got[:1]
            break
    lk.send("set_trace", struct.pack("<IB", DEFAULT_TRACE_MASK, 0xFF))
    frames += lk.poll(0.3)
    if a.storm is not None:
        lk.send("storm", struct.pack("<I", 0))
        lk.poll(0.3)

    raw_dir = ROOT / "results" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    raw_path = raw_dir / f"{a.name}.bin"
    raw_path.write_bytes(bytes(lk.raw))
    sha = hashlib.sha256(bytes(lk.raw)).hexdigest()
    with open(ROOT / "results" / "MANIFEST.sha256", "a") as mf:
        mf.write(f"{sha}  raw/{a.name}.bin  {len(lk.raw)} bytes  {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")

    jobs = {}
    last_seq = {}
    lost = {}
    statuses, hist_parts, app_stats, events, bstatuses = [], {}, None, [], []
    for idx, (ftype, seq, p) in enumerate(frames):
        if ftype == 0x03 and idx < n_capture:
            for task, flags, jseq, rl, sl, e16, r16 in kl.parse_jobs(p):
                jobs.setdefault(task, []).append((flags, rl, sl, e16, r16))
                if task in last_seq:
                    gap = (jseq - last_seq[task] - 1) & 0xFFFF
                    lost[task] = lost.get(task, 0) + gap
                last_seq[task] = jseq
        elif ftype == 0x02:
            statuses.append(kl.parse_status(p))
        elif ftype == 0x04:
            events += kl.parse_trace(p)
        elif ftype == 0x09:
            bstatuses.append(parse_bstatus(p))
        elif ftype == 0x08:
            if len(p) == 144 and p[0] < 4 and p[1] < 4:
                hist_parts.setdefault(p[0], []).append(p)
            elif len(p) == 40:
                app_stats = struct.unpack("<10I", p)
    summary = {"name": a.name, "secs": a.secs, "raw_sha256": sha, "raw_bytes": len(lk.raw),
               "frames_ok": lk.dec.ok, "frames_bad": lk.dec.bad, "work": a.work, "inject": a.inject,
               "stall_ms": a.stall_ms, "storm_hz": a.storm, "tasks": {}}
    inv = {v: k for k, v in TASK_IDS.items()}
    for task, recs in sorted(jobs.items()):
        summary["tasks"][inv.get(task, str(task))] = {
            "records": len(recs),
            "records_lost_in_transit_or_dropped": lost.get(task, 0),
            "misses_flagged": sum(1 for r in recs if r[0] & 1),
            "saturated": sum(1 for r in recs if r[0] & 4),
            "release_latency_us": summarize([r[1] for r in recs], US_PER_TICK),
            "start_latency_us": summarize([r[2] for r in recs], US_PER_TICK),
            "exec_us": summarize([r[3] * 16 for r in recs], US_PER_TICK),
            "response_us": summarize([r[4] * 16 for r in recs], US_PER_TICK),
        }
    summary["histograms"] = {HIST_NAMES[k]: hist_percentiles(parse_hist_parts(v)) for k, v in hist_parts.items()}
    if statuses:
        st = statuses[-1]
        summary["status_last"] = {k: v for k, v in st.items() if k != "tasks"}
        summary["status_tasks"] = st["tasks"]
        if len(statuses) >= 2:
            s0, s1 = statuses[0], statuses[-1]
            dt = (s1["uptime_s"] - s0["uptime_s"])
            util = {}
            for t0, t1 in zip(s0["tasks"], s1["tasks"]):
                util[inv.get(t1["id"], "idle" if t1["id"] == 8 else str(t1["id"]))] = (t1["run_cycles"] - t0["run_cycles"]) / (dt * 48e6) if dt > 0 else None
            summary["cpu_share"] = util
    if len(bstatuses) >= 2:
        b0, b1 = bstatuses[0], bstatuses[-1]
        dt = (b1["uptime_ticks"] - b0["uptime_ticks"]) / 48e6
        summary["system"] = b1["name"]
        summary["baseline_status_last"] = b1
        if dt > 0:
            summary["cpu_share"] = {inv[i]: (b1["tasks"][i]["run_cyc"] - b0["tasks"][i]["run_cyc"]) / (dt * 48e6)
                                    for i in range(6)}
    if app_stats:
        summary["app_stats"] = dict(zip(["violations", "stale_sensor", "stale_control", "mb_busy", "health_runs",
                                         "logger_loops", "adc_gaps", "mb_sensor_retries", "mb_ctrl_retries",
                                         "adc_isr"], app_stats))
    summary["trace_events"] = [(ts, kl.TE.get(t, t), i, d) for ts, t, i, d in events[:2000]]
    out = ROOT / "results" / f"{a.name}.json"
    out.write_text(json.dumps(summary, indent=1))
    brief = {k: summary[k] for k in ("frames_ok", "frames_bad")}
    print(json.dumps(brief))
    for t, v in summary["tasks"].items():
        print(f"{t:8s} n={v['records']:6d} lost={v['records_lost_in_transit_or_dropped']:5d} miss={v['misses_flagged']:4d} "
              f"start_us p50={v['start_latency_us'].get('p50', 0):.2f} p99={v['start_latency_us'].get('p99', 0):.2f} "
              f"max={v['start_latency_us'].get('max', 0):.2f}  exec_us p50={v['exec_us'].get('p50', 0):.1f} "
              f"max={v['exec_us'].get('max', 0):.1f}  resp max={v['response_us'].get('max', 0):.1f}")
    for k, v in summary["histograms"].items():
        print(k, v)
    if "cpu_share" in summary:
        print("cpu_share", {k: round(v, 4) for k, v in summary["cpu_share"].items() if v is not None})
    if "status_last" in summary:
        s = summary["status_last"]
        print({k: s[k] for k in ("uptime_s", "boot_count", "reset_cause", "trace_dropped", "jobs_dropped",
                                 "max_alarm_cyc", "max_sched_cyc", "max_crit_cyc", "dwt_rearms", "degraded",
                                 "wd_warnings", "faults", "tx_ring_full")})
    print("wrote", out)


if __name__ == "__main__":
    main()
