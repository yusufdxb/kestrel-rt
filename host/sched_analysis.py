#!/usr/bin/env python3
"""Fixed-priority schedulability analysis for the Kestrel benchmark task set.

Model (docs/SCHEDULER.md):
  - independent periodic tasks, constrained deadlines D <= T, unique fixed priorities;
  - C_i = measured WCET of one job (kernel accounting: includes interrupt time that hit the job);
  - per-job kernel overhead: one release (timer ISR share) + two context switches (PendSV) + the
    job-end system call, charged to the job's own C (standard RTA overhead inflation);
  - interrupt sources are sporadic interferers with minimum inter-arrival and WCET;
  - blocking B_i = longest non-preemptible section a lower-priority context can impose (kernel
    critical section or the longest lower-priority ISR that cannot be preempted by the timer).

Tests reported, and what they mean:
  - Liu & Layland U <= n(2^(1/n) - 1): SUFFICIENT for rate-monotonic with D = T. Failing it proves
    nothing; passing it guarantees schedulability under the model.
  - Hyperbolic bound prod(U_i + 1) <= 2: SUFFICIENT, tighter than L&L; same caveat.
  - Response-time analysis (Joseph & Pandya / Audsley et al.): EXACT (necessary and sufficient)
    for this model. Disagreement with hardware therefore means the model is wrong (a missing
    interferer, an underestimated C, a longer blocking term), not that the test is loose.
"""
import json
import math
import sys


def liu_layland(n):
    return n * (2 ** (1.0 / n) - 1)


def rta(tasks, interrupts, overhead_cyc, blocking_cyc, max_iter=1000):
    """tasks: list of dict(name, C, T, D, prio) in cycles; returns dict name -> (R or None, ok)."""
    res = {}
    for t in tasks:
        hp = [u for u in tasks if u["prio"] > t["prio"]]
        c = t["C"] + overhead_cyc
        r = c + blocking_cyc
        for _ in range(max_iter):
            nxt = c + blocking_cyc
            nxt += sum(math.ceil(r / u["T"]) * (u["C"] + overhead_cyc) for u in hp)
            nxt += sum(math.ceil(r / i["T"]) * i["C"] for i in interrupts)
            if nxt == r:
                break
            r = nxt
            if r > t["D"] * 50:
                r = None
                break
        res[t["name"]] = {"R_cycles": r, "R_us": None if r is None else r / 48.0, "D_us": t["D"] / 48.0,
                          "schedulable": r is not None and r <= t["D"]}
    return res


def analyse(tasks, interrupts, overhead_cyc, blocking_cyc):
    periodic = [t for t in tasks if t["T"]]
    u = sum(t["C"] / t["T"] for t in periodic)
    u_ovh = sum((t["C"] + overhead_cyc) / t["T"] for t in periodic) + sum(i["C"] / i["T"] for i in interrupts)
    n = len(periodic)
    hyper = math.prod(t["C"] / t["T"] + 1 for t in periodic)
    return {
        "U_tasks": u, "U_with_overheads_and_isrs": u_ovh,
        "liu_layland_bound": liu_layland(n), "liu_layland_pass (sufficient only)": u_ovh <= liu_layland(n),
        "hyperbolic_product": hyper, "hyperbolic_pass (sufficient only)": hyper <= 2.0,
        "rta (exact for the model)": rta(periodic, interrupts, overhead_cyc, blocking_cyc),
    }


def from_experiment(path, overhead_cyc, blocking_cyc, isr_cyc):
    """Build the task set from a results/*.json of host/experiment.py (measured WCET per task)."""
    d = json.load(open(path))
    prio = {"control": 7, "safety": 6, "sensor": 5, "comms": 3, "health": 2}
    period_us = {"control": 1000, "safety": 2000, "sensor": 2000, "comms": 10000, "health": 100000}
    wcet = {}
    for st in d.get("status_tasks", []):
        name = {0: "control", 1: "safety", 2: "sensor", 3: "comms", 4: "health"}.get(st["id"])
        if name:
            wcet[name] = st["wcet_cyc"]
    tasks = [{"name": n, "C": wcet.get(n, 0), "T": period_us[n] * 48, "D": period_us[n] * 48, "prio": prio[n]}
             for n in prio]
    # ADC: GPT2 trigger ISR + scan-end ISR at 2 kHz; UART RX/TEI/DMA-end ISRs bounded by line rate
    interrupts = [{"name": "adc_pair", "C": 2 * isr_cyc, "T": 24000},
                  {"name": "uart_tx_chunk_end", "C": 2 * isr_cyc, "T": 48000 // 4}]
    return analyse(tasks, interrupts, overhead_cyc, blocking_cyc)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)
    ovh = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
    blk = int(sys.argv[3]) if len(sys.argv) > 3 else 900
    isr = int(sys.argv[4]) if len(sys.argv) > 4 else 300
    print(json.dumps(from_experiment(sys.argv[1], ovh, blk, isr), indent=1))
