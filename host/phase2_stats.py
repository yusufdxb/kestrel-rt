#!/usr/bin/env python3
"""Read the RA4's Phase 2 link evidence over the USB/SCI9 path (never through the ESP link).

    python3 host/phase2_stats.py [--json]
Library: link_stats(lk) -> dict, link_outages(lk) -> (n, [(start_ms, end_ms)]), tasks(lk) -> list.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

FIELDS = ["phase2_link", "epoch", "session", "degraded", "sessions", "frames_ok", "dec_crc", "dec_len",
          "dec_version", "dec_skipped", "rej_type", "rej_len", "rej_rate", "rej_nosession", "rej_session",
          "rej_replay", "rej_jump", "rej_epoch", "rej_retired", "rej_hello_seq", "pings", "config_applied",
          "config_rejected", "budget_hit_bytes", "budget_hit_frames", "budget_hit_time", "max_job_ticks",
          "tx_frames", "tx_skipped_busy", "loss_events", "outage_total_ms", "last_valid_ms", "n_outages",
          "rx_bytes", "rx_overrun_bytes", "rx_overrun_events", "rx_hw_errors", "rx_rearms", "tx_bytes",
          "tx_refused_busy", "tx_chunks", "uptime_ms", "health_units", "logger_units", "rx_storm_mutes", "tx_dropped"]


def _result(lk, cmd, size, tries=3):
    for _ in range(tries):
        lk.send(cmd)
        f = lk.wait_for(0x08, 1.5, lambda f: len(f[2]) == size)
        if f:
            return f[2]
    return None


def link_stats(lk):
    p = _result(lk, 0x40, 4 * len(FIELDS))
    if p is None:
        return None
    d = dict(zip(FIELDS, struct.unpack(f"<{len(FIELDS)}I", p)))
    d["max_job_us"] = round(d["max_job_ticks"] / 48.0, 1)
    return d


def link_outages(lk):
    p = _result(lk, 0x41, 4 + 8 * 16)
    if p is None:
        return None
    n = struct.unpack_from("<I", p)[0]
    iv = [struct.unpack_from("<II", p, 4 + 8 * i) for i in range(16)]
    return n, iv


def tasks(lk):
    lk.send("get_tasks")
    f = lk.wait_for(0x07, 2.0)
    return kl.parse_tasks(f[2]) if f else None


if __name__ == "__main__":
    lk = kl.Link()
    s = link_stats(lk)
    o = link_outages(lk)
    t = tasks(lk)
    out = {"link": s, "outages": o, "tasks": t}
    print(json.dumps(out, indent=1) if "--json" in sys.argv else out)
