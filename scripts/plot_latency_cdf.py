#!/usr/bin/env python3
"""Control-task start latency CDFs (task-observed) for the four systems, from the raw captures.
Palette: validated categorical slots 2-5 (no blue), line dash as secondary encoding, direct labels."""
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import kestrel_link as kl  # noqa: E402

RUNS = [("Kestrel", "cmp_kestrel_v2", "#eb6834", "-"), ("FreeRTOS 11.1.0", "cmp_freertos", "#1baf7a", "--"),
        ("Zephyr 4.2.0", "cmp_zephyr", "#eda100", "-."), ("Superloop", "cmp_superloop", "#e87ba4", ":")]


def samples(name):
    d = kl.Decoder()
    out = []
    for t, s, p in d.feed((ROOT / "results/raw" / f"{name}.bin").read_bytes()):
        if t == 0x03:
            out += [sl / 48.0 for task, fl, sq, rl, sl, e, r in kl.parse_jobs(p) if task == 0]
    return sorted(out)


fig, ax = plt.subplots(figsize=(7.2, 4.6), dpi=160)
labels = []
for label, run, color, ls in RUNS:
    v = samples(run)
    n = len(v)
    ax.plot(v, [(i + 1) / n for i in range(n)], color=color, linestyle=ls, linewidth=2)
    labels.append(f"{label}: p50 {v[n // 2]:.1f}, p99 {v[int(0.99 * n)]:.1f}, max {v[-1]:.1f} us  (n={n})")
ax.set_xlabel("control task start latency after ideal release (us), task-observed", fontsize=9, color="#2b2b2b")
ax.set_ylabel("fraction of jobs", fontsize=9, color="#2b2b2b")
ax.set_title("Control task, same workload and board (RA4M1 @ 48 MHz), full raw capture per system",
             fontsize=9, color="#2b2b2b", loc="left")
ax.grid(color="#e4e2dc", linewidth=0.6)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
for s in ("left", "bottom"):
    ax.spines[s].set_color("#9a978d")
ax.tick_params(colors="#5c5a53", labelsize=8)
ax.set_xlim(0, 66)
ax.set_ylim(0, 1.01)
ax.legend(labels, fontsize=7.5, frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.16), ncol=1)
fig.tight_layout()
for ext in ("png", "svg"):
    fig.savefig(ROOT / "docs/figures" / f"start_latency_cdf.{ext}")
print("ok")
