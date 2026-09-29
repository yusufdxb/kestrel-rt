#!/usr/bin/env python3
"""CPU-utilization sweep through and beyond the schedulability boundary.

    python3 host/overload_sweep.py NAME --scales 1 2 3 4 4.5 5 5.5 6 --secs 8 [--system kestrel]

Each step multiplies every periodic task's work units by `scale` (base units: control 20,
safety 10, sensor 10, health 50), measures for --secs, and records per task: jobs, deadline misses,
max response, and (Kestrel) the RTA prediction computed from that step's measured WCETs. The
predicted boundary (first step where RTA says unschedulable) is compared with the measured one
(first step with any miss). Output: results/NAME.json (+ raw captures via experiment.py).
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import sched_analysis as sa  # noqa: E402

BASE = {"control": 20, "safety": 10, "sensor": 10, "health": 50}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--scales", nargs="+", type=float, required=True)
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--system", default="kestrel")
    ap.add_argument("--overhead", type=int, default=1500)
    ap.add_argument("--blocking", type=int, default=900)
    ap.add_argument("--isr", type=int, default=300)
    a = ap.parse_args()
    steps = []
    for sc in a.scales:
        work = [f"{k}={max(1, round(v * sc))}" for k, v in BASE.items()]
        tag = f"{a.name}_x{sc:g}"
        subprocess.run([sys.executable, str(ROOT / "host/experiment.py"), tag, "--secs", str(a.secs), "--work", *work],
                       check=True, capture_output=True)
        d = json.loads((ROOT / "results" / f"{tag}.json").read_text())
        step = {"scale": sc, "work": work, "tasks": {}}
        for t, v in d["tasks"].items():
            step["tasks"][t] = {"jobs": v["records"], "misses": v["misses_flagged"],
                                "resp_max_us": v["response_us"].get("max"), "resp_p99_us": v["response_us"].get("p99"),
                                "start_p99_us": v["start_latency_us"].get("p99"), "exec_max_us": v["exec_us"].get("max")}
        if a.system == "kestrel" and "status_tasks" in d:
            step["kernel_misses"] = {str(x["id"]): x["misses"] for x in d["status_tasks"]}
            step["kernel_overruns"] = {str(x["id"]): x["overruns"] for x in d["status_tasks"]}
            an = sa.from_experiment(str(ROOT / "results" / f"{tag}.json"), a.overhead, a.blocking, a.isr)
            step["U_with_overheads"] = an["U_with_overheads_and_isrs"]
            step["liu_layland_pass"] = an["liu_layland_pass (sufficient only)"]
            step["rta"] = {k: {"R_us": v["R_us"], "ok": v["schedulable"]} for k, v in an["rta (exact for the model)"].items()}
        if "cpu_share" in d:
            step["cpu_share"] = d["cpu_share"]
        steps.append(step)
        miss = {t: v["misses"] for t, v in step["tasks"].items() if v["misses"]}
        print(f"scale {sc:5.2f}  U~{step.get('U_with_overheads', float('nan')):.3f}  misses {miss or 'none'}  "
              f"RTA ok={all(v['ok'] for v in step.get('rta', {}).values()) if 'rta' in step else 'n/a'}")
    first_measured = next((s["scale"] for s in steps if any(v["misses"] for v in s["tasks"].values())), None)
    first_predicted = next((s["scale"] for s in steps if "rta" in s and not all(v["ok"] for v in s["rta"].values())), None)
    out = {"name": a.name, "system": a.system, "steps": steps,
           "first_scale_with_measured_miss": first_measured, "first_scale_rta_unschedulable": first_predicted}
    (ROOT / "results" / f"{a.name}.json").write_text(json.dumps(out, indent=1))
    print("measured boundary at scale", first_measured, "| RTA boundary at scale", first_predicted)


if __name__ == "__main__":
    main()
