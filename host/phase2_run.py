#!/usr/bin/env python3
"""One Phase 2 measurement: RA4 capture (host/experiment.py, USB/SCI9 path) while an ESP32-S3 fault
or Wi-Fi load runs, then RA4 link evidence and ESP counters as before/after deltas.

    python3 host/phase2_run.py NAME --secs 30 [--esp "fault malformed 15000 2000"] [--at 5]
                               [--flood-mbps 20 --flood-secs 20] [--no-esp]
Writes results/phase2/NAME.json. RA4 kernel counters (misses, overruns, boot count, watchdog) are the
authority; the job-record stream may have gaps while the ESP (which carries USB) reboots.
"""
import argparse
import json
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esp_ctrl  # noqa: E402
import kestrel_link as kl  # noqa: E402
import phase2_stats as p2  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "results" / "phase2"


def ra4_snapshot(retries=20):
    for _ in range(retries):
        try:
            lk = kl.Link()
            s = p2.link_stats(lk)
            o = p2.link_outages(lk)
            st = None
            end = time.time() + 2.0
            while time.time() < end and st is None:
                for t, _, p in lk.poll(0.2):
                    if t == 0x02:
                        st = kl.parse_status(p)
            lk.ser.close()
            if s and st:
                return {"link": s, "outages": o, "status": {k: v for k, v in st.items() if k != "tasks"},
                        "tasks": st["tasks"]}
        except (Exception, SystemExit) as ex:  # noqa: BLE001  (USB bridge may be re-enumerating; find_port exits)
            print("ra4 snapshot retry:", ex)
        time.sleep(1.5)
    return None


def esp_snapshot(timeout=60.0, rejoin=True):
    e = esp_ctrl.Esp(timeout=0.8)
    end = time.time() + timeout
    rejoined = False
    while time.time() < end:
        s = e.stats()
        if s:
            return s
        if rejoin and not rejoined and time.time() > end - timeout + 8:
            subprocess.run([str(ROOT / "scripts/esp_wifi.sh"), "up"], capture_output=True)
            rejoined = True
        time.sleep(1.0)
    return None


def flood(mbps, secs, stats):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = b"\xA5" * 1400
    end = time.time() + secs
    gap = 0.0 if mbps <= 0 else 1400 * 8 / (mbps * 1e6)
    nxt = time.time()
    while time.time() < end:
        try:
            s.sendto(payload, (esp_ctrl.ESP_IP, esp_ctrl.SINK_PORT))
            stats["sent"] += 1
        except OSError:
            stats["err"] += 1
            time.sleep(0.001)
        if gap:
            nxt += gap
            d = nxt - time.time()
            if d > 0:
                time.sleep(d)


def delta(a, b):
    return {k: b[k] - a[k] for k in b if isinstance(b.get(k), int) and isinstance(a.get(k), int)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--secs", type=float, default=30.0)
    ap.add_argument("--esp", action="append", default=[], help="UDP command(s) sent at --at")
    ap.add_argument("--at", type=float, default=5.0)
    ap.add_argument("--flood-mbps", type=float, default=None)
    ap.add_argument("--flood-secs", type=float, default=20.0)
    ap.add_argument("--no-esp", action="store_true", help="no ESP counters (stock bridge / B0)")
    ap.add_argument("--settle", type=float, default=3.0)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    pre_ra4 = ra4_snapshot()
    pre_esp = None if a.no_esp else esp_snapshot(20)
    exp = subprocess.Popen([sys.executable, str(ROOT / "host/experiment.py"), f"p2_{a.name}", "--secs", str(a.secs)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    fl = {"sent": 0, "err": 0}
    replies = []
    t0 = time.time()
    time.sleep(1.5 + a.at)  # experiment.py warmup + offset
    th = None
    if a.flood_mbps is not None:
        th = threading.Thread(target=flood, args=(a.flood_mbps, a.flood_secs, fl), daemon=True)
        th.start()
    e = esp_ctrl.Esp(timeout=1.0)
    for c in a.esp:
        replies.append((round(time.time() - t0, 2), c, e.cmd(c, tries=1)))
    out_txt = exp.communicate()[0]
    if th:
        th.join()
    time.sleep(a.settle)
    post_ra4 = ra4_snapshot()
    post_esp = None if a.no_esp else esp_snapshot(90)
    exp_json = json.loads((ROOT / "results" / f"p2_{a.name}.json").read_text())
    ctl = exp_json["tasks"].get("control", {})
    res = {
        "name": a.name, "secs": a.secs, "esp_cmds": replies, "flood": fl if a.flood_mbps is not None else None,
        "flood_mbps_target": a.flood_mbps,
        "ra4_boot_before": pre_ra4["status"]["boot_count"] if pre_ra4 else None,
        "ra4_boot_after": post_ra4["status"]["boot_count"] if post_ra4 else None,
        "kernel_tasks_after": post_ra4["tasks"] if post_ra4 else None,
        "kernel_status_after": post_ra4["status"] if post_ra4 else None,
        "control_start_us": ctl.get("start_latency_us"), "control_records": ctl.get("records"),
        "control_records_lost": ctl.get("records_lost_in_transit_or_dropped"),
        "tasks_capture": {t: {"n": v["records"], "miss": v["misses_flagged"], "start_us": v["start_latency_us"],
                              "resp_us": v["response_us"]} for t, v in exp_json["tasks"].items()},
        "cpu_share": exp_json.get("cpu_share"), "raw_sha256": exp_json["raw_sha256"],
        "link_delta": delta(pre_ra4["link"], post_ra4["link"]) if pre_ra4 and post_ra4 else None,
        "link_after": post_ra4["link"] if post_ra4 else None,
        "outages_after": post_ra4["outages"] if post_ra4 else None,
        "esp_before": pre_esp, "esp_after": post_esp,
        "esp_delta": delta(pre_esp, post_esp) if pre_esp and post_esp else None,
        "experiment_stdout": out_txt[-3000:],
    }
    kt = res["kernel_tasks_after"] or []
    res["kernel_misses"] = {str(t["id"]): t["misses"] for t in kt}
    res["critical_misses"] = sum(t["misses"] for t in kt if t["id"] in (0, 1, 2))
    (OUT / f"{a.name}.json").write_text(json.dumps(res, indent=1, default=str))
    print(json.dumps({k: res[k] for k in ("name", "ra4_boot_before", "ra4_boot_after", "critical_misses",
                                           "kernel_misses", "control_records", "control_records_lost")}))
    print("control start us", {k: round(v, 2) for k, v in (res["control_start_us"] or {}).items() if k != "n"})
    ld = res["link_delta"] or {}
    print("link delta", {k: v for k, v in ld.items() if v})
    if res["esp_delta"]:
        print("esp delta", {k: v for k, v in res["esp_delta"].items() if v and k not in ("up_ms",)})
    print("esp cmds", replies)


if __name__ == "__main__":
    main()
