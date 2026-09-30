#!/usr/bin/env python3
"""Short interactive demo on the physical board (a walkthrough, not validation evidence).

    python3 host/demo.py [--elf build/phase2/kestrel.elf] [--secs 8] [--esp]

1. timing: a short capture of per-job records (host/experiment.py), per-task latency and CPU share
2. fault: one contained MPU fault (NULL write in the unprivileged logger task); the task is parked,
   the crash record is read back, control keeps its deadlines; then a software reset clears it
3. --esp (Phase 2 image + host joined to the ESP SoftAP): force an ESP32-S3 panic and show that the
   RA4M1 did not reset, missed no critical deadline, and accepted a new link session
Writes only results/demo_run.json and results/raw/demo_run.bin (both gitignored); the raw-data
manifest is left unchanged. The measured campaigns are docs/TEST_PLAN.md and
docs/PHASE2_RESULTS.md.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fault_campaign as fc  # noqa: E402
import kestrel_link as kl  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CRIT = (0, 1, 2)  # control, safety, sensor


def head(t):
    print(f"\n== {t}")


def timing(secs):
    head(f"1. timing: {secs} s of per-job records")
    manifest = ROOT / "results" / "MANIFEST.sha256"
    kept = manifest.read_bytes() if manifest.exists() else None
    out = subprocess.run([sys.executable, str(ROOT / "host/experiment.py"), "demo_run", "--secs", str(secs)],
                         capture_output=True, text=True)
    if kept is not None:
        manifest.write_bytes(kept)  # demo captures are not evidence: keep them out of the raw-data manifest
    keep = ("control", "safety", "sensor", "comms", "health", "logger", "link", "cpu_share")
    for line in out.stdout.splitlines():
        if line.startswith(keep):
            print("  " + line.replace("'6':", "'link':"))
    if out.returncode != 0:
        print(out.stderr[-600:])
        sys.exit("capture failed")


def fault(elf):
    head("2. fault: NULL write in the unprivileged logger task (MPU)")
    fc.ELF = Path(elf)
    case = next(c for c in fc.CASES if c[0] == "null_write_contained")
    lk = kl.Link()
    lk.poll(0.5)
    r = fc.run_case(lk, fc.symbols(), case)
    print(f"  verdict {r['verdict']}: boot {r.get('boot_before')} -> {r.get('boot_after')} (no reset), "
          f"logger state {r.get('task_state')} (3 = FAULTED), control jobs +{r.get('control_jobs_delta')}, "
          f"control misses +{r.get('control_misses')}")
    if "crash_kind" in r:
        print(f"  crash record: {r['crash_kind']} {' '.join(r['cfsr_bits'])}, MMFAR {r['mmfar']}, "
              f"PC {r['pc']} in {r['pc_func']}, captured in {r['capture_cycles']} cycles")
    lk.send("reset")  # parked tasks stay parked until reset
    lk.ser.close()
    time.sleep(3.0)
    return r["verdict"] == "PASS"


def esp_crash():
    import esp_ctrl
    import phase2_run as pr
    head("3. Phase 2: force an ESP32-S3 panic")
    a = pr.ra4_snapshot()
    if a is None or esp_ctrl.Esp(timeout=1.0).stats() is None:
        print("  RA4 link stats or ESP SoftAP not reachable (Phase 2 image? scripts/esp_wifi.sh up?)")
        return False
    print("  ESP reply:", esp_ctrl.Esp(timeout=1.0).cmd("fault crash", tries=1))
    time.sleep(12.0)  # ESP reboots, USB re-enumerates, new mailbox session
    b = pr.ra4_snapshot()
    if b is None:
        print("  RA4 not readable after the ESP reboot (USB bridge); RA4 counters are checked on the next run")
        return False
    miss = sum(x["misses"] for x in b["tasks"] if x["id"] in CRIT) - sum(x["misses"] for x in a["tasks"] if x["id"] in CRIT)
    jobs = b["tasks"][0]["jobs"] - a["tasks"][0]["jobs"]
    ok = b["status"]["boot_count"] == a["status"]["boot_count"] and miss == 0 and b["link"]["sessions"] > a["link"]["sessions"]
    print(f"  RA4 boot {a['status']['boot_count']} -> {b['status']['boot_count']}, control jobs +{jobs}, "
          f"critical misses +{miss}, link sessions {a['link']['sessions']} -> {b['link']['sessions']}, "
          f"outages +{b['link']['loss_events'] - a['link']['loss_events']}")
    print("  ", "PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ROOT / "build/phase2/kestrel.elf"))
    ap.add_argument("--secs", type=int, default=8)
    ap.add_argument("--esp", action="store_true")
    a = ap.parse_args()
    timing(a.secs)
    ok = fault(a.elf)
    if a.esp:
        ok &= esp_crash()
    print("\ndemo", "OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
