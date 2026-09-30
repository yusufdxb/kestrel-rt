#!/usr/bin/env python3
"""G9 dual-processor soak: RA4 workload + link under normal Wi-Fi telemetry, forced ESP crash every
--crash-every s. Evidence comes over two independent paths:
  Wi-Fi: every RA4 HEARTBEAT/STATUS/METRICS forwarded by the ESP (logged, decoded),
  USB:   RA4 kernel + link counters every --usb-every s (best effort; the bridge may wedge).
Stops when the RA4 control job count (HEARTBEAT) has advanced by --periods.
    python3 host/phase2_soak.py soak_p2 --periods 10000000 --crash-every 900
"""
import argparse
import json
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esp_ctrl  # noqa: E402
import phase2_run as pr  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--periods", type=int, default=10_000_000)
    ap.add_argument("--crash-every", type=float, default=900)
    ap.add_argument("--usb-every", type=float, default=300)
    a = ap.parse_args()
    out = ROOT / "results" / "phase2"
    log = open(out / f"{a.name}_udp.jsonl", "a")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    first = pr.ra4_snapshot()
    esp0 = pr.esp_snapshot(30)
    t0 = time.time()
    hb0 = None
    last = {}
    crashes = []
    usb = []
    next_sub = 0
    next_crash = t0 + a.crash_every
    next_usb = t0 + a.usb_every
    hb_count = 0
    max_hb_gap = 0.0
    last_hb_t = None
    epochs = set()
    while True:
        now = time.time()
        if now >= next_sub:
            try:
                s.sendto(b"sub", (esp_ctrl.ESP_IP, esp_ctrl.CTRL_PORT))
            except OSError:
                pass
            next_sub = now + 5
        if now >= next_crash:
            crashes.append({"t": round(now - t0, 1), "reply": esp_ctrl.Esp(timeout=1.0).cmd("fault crash", tries=1)})
            next_crash = now + a.crash_every
        if now >= next_usb:
            snap = pr.ra4_snapshot(retries=2)
            usb.append({"t": round(now - t0, 1), "ok": snap is not None,
                        "boot": snap and snap["status"]["boot_count"],
                        "crit_misses": snap and sum(x["misses"] for x in snap["tasks"] if x["id"] in (0, 1, 2)),
                        "ctl_jobs": snap and snap["tasks"][0]["jobs"],
                        "sessions": snap and snap["link"]["sessions"], "loss": snap and snap["link"]["loss_events"]})
            log.write(json.dumps({"usb": usb[-1]}) + "\n")
            log.flush()
            next_usb = now + a.usb_every
        try:
            d, _ = s.recvfrom(2048)
        except (socket.timeout, OSError):
            continue
        if d[:2] != b"KX" or len(d) < 14:
            continue
        typ = d[3]
        n = struct.unpack_from("<H", d, 10)[0]
        p = d[12:12 + n]
        if typ == 0x85 and n >= 23:
            ep, ms, ses, lseq, jobs, miss, deg = struct.unpack_from("<IIHIIIB", p)
            if last_hb_t is not None:
                max_hb_gap = max(max_hb_gap, now - last_hb_t)
            last_hb_t = now
            hb_count += 1
            epochs.add(ep)
            if hb0 is None:
                hb0 = {"jobs": jobs, "miss": miss, "ep": ep, "ms": ms}
            last = {"ep": ep, "ms": ms, "session": ses, "jobs": jobs, "miss": miss, "degraded": deg}
            if hb_count % 600 == 0:
                log.write(json.dumps({"t": round(now - t0, 1), "hb": last}) + "\n")
                log.flush()
                print(f"{now - t0:8.0f}s ctl_jobs+{jobs - hb0['jobs']:,} ctl_miss {miss} epoch {ep} crashes {len(crashes)}", flush=True)
            if jobs - hb0["jobs"] >= a.periods:
                break
        elif typ == 0x84 and n >= 44:
            m = struct.unpack_from("<11I", p)
            log.write(json.dumps({"t": round(now - t0, 1), "metrics": m}) + "\n")
    time.sleep(3)
    final = pr.ra4_snapshot()
    esp1 = pr.esp_snapshot(60)
    res = {"name": a.name, "wall_s": round(time.time() - t0, 1), "periods_target": a.periods,
           "hb_first": hb0, "hb_last": last, "control_periods_wifi": last["jobs"] - hb0["jobs"],
           "ra4_epochs_seen": sorted(epochs), "heartbeats": hb_count, "max_heartbeat_gap_s": round(max_hb_gap, 2),
           "esp_crashes_forced": crashes, "usb_checks": usb, "ra4_before": first, "ra4_after": final,
           "esp_before": esp0, "esp_after": esp1}
    (out / f"{a.name}.json").write_text(json.dumps(res, indent=1, default=str))
    print(json.dumps({k: res[k] for k in ("wall_s", "control_periods_wifi", "ra4_epochs_seen", "heartbeats",
                                           "max_heartbeat_gap_s")}), "crashes", len(crashes))


if __name__ == "__main__":
    main()
