#!/usr/bin/env python3
"""Kestrel-RT walkthrough on the physical board. Every LIVE line is read from the board during the run;
the closing VALIDATED block is read from the recorded campaign result files, never generated.

    python3 host/demo.py [--elf build/phase2/kestrel.elf] [--secs 8] [--esp]

Part 1 (USB): runtime state, a short timing capture, one contained MPU fault, designed recovery.
Part 2 (--esp; Phase 2 images, host joined to the ESP SoftAP): live RA4 heartbeats over Wi-Fi, a forced
ESP32-S3 panic, the gap while the ESP is gone, the RA4's own counters across that gap, the new session.
Writes only gitignored files (results/demo_run.json, results/raw/demo_run.bin); the raw-data manifest is
left unchanged.
"""
import argparse
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fault_campaign as fc  # noqa: E402
import kestrel_link as kl  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CRIT = (0, 1, 2)  # control, safety, sensor
NAMES = {0: "control", 1: "safety", 2: "sensor", 3: "comms", 4: "health", 5: "logger", 6: "link"}
STATE = {0: "unused", 1: "ready", 2: "waiting", 3: "FAULTED"}


def stage(t):
    print(f"\n-- {t} " + "-" * max(0, 66 - len(t)), flush=True)


def live(msg):
    print(f"  [live] {msg}", flush=True)


def status(lk, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        for t, _, p in lk.poll(0.1):
            if t == 0x02:
                return kl.parse_status(p)
    return None


def runtime_state():
    stage("runtime on the RA4M1 (USB)")
    lk = kl.Link()
    st = status(lk)
    lk.ser.close()
    if st is None:
        sys.exit("no status frame from the board")
    live(f"boot #{st['boot_count']}, last reset: {kl.reset_str(st['reset_cause'])}, "
         f"uptime {st['uptime_s']:.1f} s, faults {st['faults']}, watchdog warnings {st['wd_warnings']}")
    for t in st["tasks"]:
        if t["id"] == 5:
            live(f"logger   background (no period)  stack {t['stack_used']:>4} B  {STATE.get(t['state'], t['state'])}")
        elif t["id"] in NAMES:
            live(f"{NAMES[t['id']]:8s} jobs {t['jobs']:>9,}  misses {t['misses']}  "
                 f"stack {t['stack_used']:>4} B  {STATE.get(t['state'], t['state'])}")


def timing(secs):
    stage(f"timing: {secs} s of per-job records streamed from the kernel")
    manifest = ROOT / "results" / "MANIFEST.sha256"
    kept = manifest.read_bytes() if manifest.exists() else None
    out = subprocess.run([sys.executable, str(ROOT / "host/experiment.py"), "demo_run", "--secs", str(secs)],
                         capture_output=True, text=True)
    if kept is not None:
        manifest.write_bytes(kept)  # demo captures are not evidence: keep them out of the raw-data manifest
    if out.returncode != 0:
        print(out.stderr[-600:])
        sys.exit("capture failed")
    s = json.loads((ROOT / "results" / "demo_run.json").read_text())
    print("           task      jobs  misses  start p50 / p99 / max (us)   CPU")
    cpu = s.get("cpu_share", {})
    for name in ("control", "safety", "sensor", "comms", "health"):
        v = s["tasks"].get(name)
        if not v:
            continue
        sl = v["start_latency_us"]
        live(f"{name:8s} {v['records']:>7,}  {v['misses_flagged']:>6}  {sl.get('p50', 0):7.2f} / {sl.get('p99', 0):6.2f} / "
             f"{sl.get('max', 0):6.2f}  {100 * (cpu.get(name) or 0):5.1f}%")


def fault(elf):
    stage("fault: NULL write in the unprivileged logger task")
    fc.ELF = Path(elf)
    case = next(c for c in fc.CASES if c[0] == "null_write_contained")
    lk = kl.Link()
    lk.poll(0.5)
    r = fc.run_case(lk, fc.symbols(), case)
    if "crash_kind" in r:
        live(f"MPU trapped it: {r['crash_kind']} {' '.join(r['cfsr_bits'])}, fault address {r['mmfar']}, "
             f"PC {r['pc']} ({r['pc_func']})")
        live(f"crash record captured in {r['capture_cycles']} cycles ({r['capture_cycles'] / 48:.0f} us), read back over USB")
    live(f"logger parked {STATE.get(r.get('task_state'), r.get('task_state'))}; board not reset "
         f"(boot #{r.get('boot_before')} -> #{r.get('boot_after')})")
    live(f"control kept running: +{r.get('control_jobs_delta')} jobs, +{r.get('control_misses')} misses")
    stage("designed recovery: host-requested software reset restarts the parked task")
    lk.send("reset")
    lk.ser.close()
    time.sleep(3.0)
    lk = kl.Link()
    st = status(lk)
    lk.ser.close()
    if st:
        lg = next(t for t in st["tasks"] if t["id"] == 5)
        live(f"boot #{st['boot_count']}, reset: {kl.reset_str(st['reset_cause'])}, logger {STATE.get(lg['state'], lg['state'])}")
    return r["verdict"] == "PASS"


def heartbeats(sock, secs, until_gap=None):
    """Read RA4 HEARTBEAT frames forwarded by the ESP. Returns list of (t, epoch, ms, session, jobs, miss)."""
    import esp_ctrl
    out, end, last_sub = [], time.time() + secs, 0.0
    while time.time() < end:
        if time.time() - last_sub > 2.0:
            try:
                sock.sendto(b"sub", (esp_ctrl.ESP_IP, esp_ctrl.CTRL_PORT))
            except OSError:
                pass
            last_sub = time.time()
        try:
            d, _ = sock.recvfrom(2048)
        except (socket.timeout, OSError):
            continue
        if d[:2] != b"KX" or len(d) < 14 or d[3] != 0x85:
            continue
        n = struct.unpack_from("<H", d, 10)[0]
        if n < 23:
            continue
        ep, ms, ses, _lseq, jobs, miss, _deg = struct.unpack_from("<IIHIIIB", d, 12)
        out.append((time.time(), ep, ms, ses, jobs, miss))
        if until_gap is not None and until_gap(out):
            break
    return out


def esp_crash():
    import esp_ctrl
    import phase2_run as pr
    stage("Phase 2: ESP32-S3 link online")
    a = pr.ra4_snapshot()
    e = esp_ctrl.Esp(timeout=1.0).stats()
    if a is None or e is None:
        print("  RA4 link stats or ESP SoftAP not reachable (Phase 2 images? scripts/esp_wifi.sh up?)")
        return False
    live(f"RA4 link: session {a['link']['session']}, {a['link']['frames_ok']:,} frames accepted, "
         f"{a['link']['sessions']} session(s) since boot, degraded {a['link']['degraded']}")
    live(f"ESP32-S3: boot #{e['boot']}, Wi-Fi client(s) {e['stations']}, PING RTT avg {e['rtt_us_avg']} us, "
         f"heap {e['heap']:,} B, {e['resyncs']} resyncs")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.settimeout(0.2)
    hb = heartbeats(s, 2.5)
    if len(hb) < 2:
        print("  no RA4 heartbeats over Wi-Fi")
        return False
    t0, ep0, _, ses0, j0, m0 = hb[-1]
    live(f"RA4 heartbeats over Wi-Fi: {len(hb)} in 2.5 s, RA4 epoch {ep0}, control jobs {j0:,}, misses {m0}")

    stage("force an ESP32-S3 panic (NULL write on the ESP)")
    live(f"ESP reply: {esp_ctrl.Esp(timeout=1.0).cmd('fault crash', tries=1)}")
    t_crash = time.time()
    # wait for the ESP to come back: heartbeats resume
    after = heartbeats(s, 30.0, until_gap=lambda o: len(o) >= 3 and o[-1][0] - t_crash > 1.0)
    resumed = [h for h in after if h[0] - t_crash > 0.05]
    if not resumed:
        print("  heartbeats did not resume within 30 s")
        return False
    gap = resumed[0][0] - t0
    t1, ep1, _, ses1, j1, m1 = resumed[-1]
    live(f"ESP gone: no telemetry for {gap:.1f} s (ESP reboot, Wi-Fi rejoin, new session)")
    live(f"telemetry resumed: RA4 epoch {ep1} ({'same RA4 boot' if ep1 == ep0 else 'RA4 REBOOTED'}), "
         f"link session {ses0} -> {ses1}")
    live(f"RA4 control ran through it: +{j1 - j0:,} jobs in {t1 - t0:.1f} s (1 ms period), misses {m0} -> {m1}")

    stage("RA4 kernel counters over USB after the ESP re-enumerates")
    time.sleep(3.0)
    b = pr.ra4_snapshot()
    if b is None:
        print("  RA4 not readable over USB yet (bridge re-enumerating); Wi-Fi evidence above stands")
        return ep1 == ep0 and m1 == m0
    miss = sum(x["misses"] for x in b["tasks"] if x["id"] in CRIT) - sum(x["misses"] for x in a["tasks"] if x["id"] in CRIT)
    n, iv = b["outages"] if b["outages"] else (0, [])
    last = iv[(n - 1) % 16] if n else None
    live(f"RA4 boot #{a['status']['boot_count']} -> #{b['status']['boot_count']}, critical misses +{miss}, "
         f"sessions {a['link']['sessions']} -> {b['link']['sessions']}")
    if last:
        live(f"RA4 outage log, latest entry: link lost for {last[1] - last[0]} ms")
    return b["status"]["boot_count"] == a["status"]["boot_count"] and miss == 0 and ep1 == ep0


def validated():
    stage("VALIDATED CAMPAIGNS (recorded results, not this run)")
    p1 = json.loads((ROOT / "results/soak_10M.json").read_text())["target_counters_delta"]
    crit1 = [t for t in p1["tasks"] if t["id"] in CRIT]
    print(f"  Phase 1 soak    {crit1[0]['jobs']:,} control periods ({p1['uptime_s'] / 3600:.1f} h), "
          f"critical misses {sum(t['misses'] for t in crit1)}            results/soak_10M.json")
    fcr = json.loads((ROOT / "results/fault_campaign_run5_final.json").read_text())
    print(f"  Fault campaign  {sum(r['verdict'] == 'PASS' for r in fcr)}/{len(fcr)} cases pass on hardware"
          f"                       results/fault_campaign_run5_final.json")
    s = json.loads((ROOT / "results/phase2/soak_p2.json").read_text())
    a, b = s["ra4_before"], s["ra4_after"]
    ctl = b["tasks"][0]["jobs"] - a["tasks"][0]["jobs"]
    miss = sum(x["misses"] for x in b["tasks"] if x["id"] in CRIT) - sum(x["misses"] for x in a["tasks"] if x["id"] in CRIT)
    print(f"  Phase 2 soak    {ctl:,} control periods, {len(s['esp_crashes_forced'])} forced ESP crashes, "
          f"RA4 resets {b['status']['boot_count'] - a['status']['boot_count']}, critical misses {miss}"
          f"   results/phase2/soak_p2.json")
    print("  Known cost      +3.3 us control start p99 with the link task present; FreeRTOS 11.1.0 had lower")
    print("                  start latency than Kestrel (docs/BENCHMARKS.md, docs/LIMITATIONS.md)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ROOT / "build/phase2/kestrel.elf"))
    ap.add_argument("--secs", type=int, default=8)
    ap.add_argument("--esp", action="store_true")
    a = ap.parse_args()
    runtime_state()
    timing(a.secs)
    ok = fault(a.elf)
    if a.esp:
        ok &= esp_crash()
    validated()
    print("\nlive demo", "OK" if ok else "FAILED", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
