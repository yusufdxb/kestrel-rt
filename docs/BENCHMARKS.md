# Benchmarks

All numbers were measured on the physical RA4M1 (Arduino UNO R4 WiFi, 48 MHz, flash at 1 wait state) with
the image and host tool named in each row. Raw byte streams are kept outside Git (`results/raw/`, sha256 in
`results/MANIFEST.sha256`); per-run summaries are the JSON files in `results/`. Latencies are in the 48 MHz
timer domain (1 tick = 20.83 ns). Percentiles from on-target histograms are bin upper edges (conservative).

## Method in one paragraph
Every system runs the same six-task workload (docs/SPEC.md section 4) with the same workload kernels
(`apps/common/workload.c`), the same lock-free mailboxes, the same ADC path and the same register-level UART
DMA telemetry. Kestrel emits 12-byte job records from the kernel; the baselines emit the identical record
format from a small shim (`baselines/common/bench.c`). Nominal release instants lie on one timebase: GPT320
(Kestrel release timer, FreeRTOS tick, superloop frame all use its compare match) or, for Zephyr, its own
48 MHz SysTick cycle domain with a tick-edge calibration. Start latency is measured by the task itself as
its first action after being released, the one definition all four systems can implement identically.

## 1. Same workload, four systems (30 s, 0.18 CPU in control, ~0.29 total periodic load)

![Start latency CDF](figures/start_latency_cdf.svg)

| Control task (1 ms) | Kestrel | FreeRTOS 11.1.0 | Zephyr 4.2.0 | Superloop |
|---|---:|---:|---:|---:|
| jobs / deadline misses | 31,320 / 0 | 31,190 / 0 | 31,150 / 0 | 31,240 / 0 |
| start latency p50 (us) | 24.6 | 15.9 | 32.7 | 32.0 |
| start latency p99 (us) | 44.9 | 18.7 | 44.6 | 54.7 |
| start latency max (us) | 49.0 | 22.3 | 54.7 | 62.1 |
| execution p50 / max (us) | 172.3 / 181.0 | 156.3 / 165.0 | 158.0 / 183.0 | 158.3 / 171.3 |
| response max (us) | 213.0 | 182.3 | 215.7 | 222.3 |

Results files: `cmp_kestrel_v2.json`, `cmp_freertos.json`, `cmp_zephyr.json`, `cmp_superloop.json`.

Reading the table honestly:
- **FreeRTOS has the lowest and tightest release-to-start latency** (p50 15.9 us, max 22.3 us). Its tick
  handler does very little per tick; Kestrel's kernel timer ISR does per-job deadline and latency bookkeeping,
  64-bit tickless timekeeping and the supervisor check (p50 416 cycles, 8.7 us), and its switch retargets the
  MPU for an unprivileged task.
- **Kestrel beats Zephyr and the superloop at p50 and matches Zephyr at p99** (44.9 vs 44.6 us).
- Kestrel's execution time includes its job-end system call (per-job record, WCET/response tracking,
  deadline check), about 500 cycles the others do not perform.
- The superloop's start latency is the length of the background chunk in progress when the frame begins
  (non-preemptive); it has no scheduler to wait for, but nothing can preempt a chunk either.

## 2. Context switch (task-to-task, microbenchmark, same method on both kernels)

B signals a higher-priority task A blocked on a flag (Kestrel event flags; FreeRTOS direct task notification,
its fastest primitive); A timestamps on wake (signal-to-wake); A blocks again and B timestamps when it
resumes (block-to-resume). One round per millisecond. 333 ns histogram bins.

| ns | signal-to-wake p50 / p99 / p99.9 / max | block-to-resume p50 / p99 / p99.9 / max | samples |
|---|---|---|---:|
| FreeRTOS 11.1.0 (notifications, no MPU) | 10,333 / 10,333 / 10,333 / 13,146 | 12,667 / 12,667 / 12,667 / 15,500 | 27,750 |
| Kestrel, unprivileged tasks, MPU on | 13,000 / 13,000 / 39,333 / 39,375 | 14,000 / 14,000 / 40,333 / 45,833 | 37,795 |
| Kestrel, privileged tasks, MPU off | 13,000 / 13,000 / 17,000 / 38,917 | 13,667 / 14,000 / 40,000 / 39,958 | 37,317 |

- MPU isolation plus unprivileged system calls cost Kestrel about 0.3 us per switch pair.
- Kestrel's 0.1% tail (up to ~40 us) is its kernel timer ISR, which deliberately runs at the highest device
  priority (to minimize release latency) and can land inside the measured path; FreeRTOS runs its tick at
  the lowest priority, so it cannot interrupt a switch. Identified with the flight recorder
  (`host/timeline.py`, ISR entry/exit events).

Kestrel kernel internals, same 30 s run (DWT cycles @ 48 MHz, on-target histograms):

| | p50 | p95 | max |
|---|---:|---:|---:|
| PendSV, first to last instruction | 316 | 352 | 654 |
| kernel timer ISR body | 416 | 984 | 1,071 |
| SVC dispatch body | 88 | (40% are job-end calls above the 512-cycle histogram range) | 549 |
| timer compare match to handler (ticks) | 84 (1.75 us) | 84 | 203 (4.2 us) |

## 3. Overload: sweep through and beyond the schedulability boundary

All periodic work scaled by a common factor; 10 s per point, each point from a fresh reset; counters read over
SWD without halting (at high load the telemetry task is starved by design). `results/sweep_*_swd.json`.

| scale | Kestrel | FreeRTOS 11.1.0 | Superloop |
|---:|---|---|---|
| 3.0 to 5.0 | 0 misses | 0 misses (health misses from 5.0) | 0 misses |
| 5.25 | comms 12, health 19 misses | (not run) | (not run) |
| 5.5 | comms/health starved; control, safety, sensor 0 | health degraded; control 0 | **control misses 41% of jobs** |
| 6.0 | sensor, safety fail; **control 0 misses** | sensor fails; **control 0 misses** | every task misses |
| 6.5 | control's own C exceeds T (estimated ~1.08 ms from the per-unit cost; not measured at this point): control misses 4,752 of 7,116 jobs | sensor, comms, health **starved (0 jobs)** | every task misses |
| 7.0 | control at half rate (skip policy, 5,911 jobs / 10 s); safety, sensor, comms **0 misses** | control misses every job; **every other task starved (0 jobs)** | every task misses |

- Fixed-priority scheduling (Kestrel and FreeRTOS) protects the control task until its own execution time
  exceeds its period; the non-preemptive superloop loses control deadlines at 5.5x.
- Beyond that point the overrun policy decides the outcome. Kestrel skips releases of a late job (no backlog),
  so the late task degrades to a lower rate and the rest keep running. FreeRTOS `vTaskDelayUntil` releases
  late jobs back to back, so the overrunning task monopolizes the CPU. (A FreeRTOS application can implement
  skipping itself using the return value of `xTaskDelayUntil`; the table shows the default pattern.)
- **Finding**: once overloaded, the lower-priority command path starves and the host can no longer reduce
  the load; the watchdog does not intervene because the critical tasks still vote. Recovery needs a reset.

### Theory vs hardware (Kestrel)
`host/sched_analysis.py` with per-step measured WCETs (`results/sweep_kestrel.json`):
- RTA predicted the failing tasks in the same priority order the board showed (health, comms, sensor, safety).
- No false-safe prediction: RTA never declared a load schedulable at which a miss was then observed.
- RTA was conservative by 0.25 to 0.75 of a scale step: WCET inputs include rare spikes (the comms task's
  once-per-second status frame costs ~3.5x its median) that do not coincide in every hyperperiod. Utilization
  computed from WCETs exceeded 1.0 (1.10 at 4.5x) while the board still met every deadline; the Liu-Layland
  and hyperbolic bounds are sufficient tests and are reported as such, never as necessary conditions.

## 4. Interrupt storm (GPT321 interrupt at priority 3, above all tasks)

| storm rate | Kestrel control start p50 / p99 / max (us) | FreeRTOS control start p50 / p99 / max (us) | misses (both) |
|---:|---|---|---:|
| 0 | 24.6 / 44.9 / 46.4 | 15.7 / 18.5 / 19.2 | 0 |
| 50 kHz | 26.8 / 46.9 / 50.7 | 16.8 / 21.7 / 25.4 | 0 |
| 100 kHz | 28.6 / 49.2 / 50.7 | 20.0 / 22.8 / 26.2 | 0 |
| 200 kHz | 30.9 / 51.0 / 55.6 | 22.1 / 27.0 / 31.0 | 0 |
| 300 kHz | 37.9 / 54.6 / 62.0 | 29.7 / 33.0 / 36.7 | 0 |

The storm mostly steals CPU (control execution p50 172 -> 482 us in Kestrel at 300 kHz); both kernels kept
every deadline. Kestrel's timer interrupt sits above the storm: its entry latency stayed at p50 1.75 us, but
1.3% of entries exceeded 5.3 us at 300 kHz (max 12.8 us), not yet explained.

## 5. Telemetry backpressure (Kestrel)
- **On-target TX stall, 5 s** (`tx_stall_kestrel.json`): control, safety, sensor 0 misses; control start p99
  unchanged (44.85 us); 10,380 job records dropped and counted on the target, equal to the host's sequence-gap
  count (every lost record accounted for).
- **Host stops reading, 16.9 s** (`host_refuse_read.json`): 0 misses on every task (16,847 control jobs, counted
  over SWD). The ESP32-S3 USB bridge's CDC path stalled and needed a hub-port cycle; the RA4M1 never reset.

## 6. Priority inversion (`apps/inversion`, same binary, 20 ms periods, ~500 samples each)
| mutex | H blocking mean | H blocking max |
|---|---:|---:|
| plain (no protocol) | 5,173 us | 5,179 us |
| immediate priority ceiling | 909 us | 943 us |

Plain: H waits for the medium task's entire 5 ms burst (unbounded inversion). Ceiling: H waits only for the
remainder of L's 1 ms critical section.

## 7. Footprint (same workload, bytes, attributed from the linker map: `scripts/footprint.py`)
| image | flash | RAM | of which the kernel |
|---|---:|---:|---|
| Kestrel | 23,046 | 20,569 | 12,291 flash / 5,967 RAM (scheduler, IPC, faults, crash log, MPU, supervisor, instrumentation) |
| Superloop | 8,158 | 7,094 | none |
| FreeRTOS 11.1.0 | 11,074 | 15,659 | 2,832 flash / 897 RAM (+ TCBs and stacks in the application) |
| Zephyr 4.2.0 | 19,763 | 20,150 | 8,596 flash / ~4.1 KB RAM (kernel + arch + lib) |

Kestrel's kernel is the largest of the three kernels in this configuration: it includes fault capture, the
crash log, the MPU policy, the watchdog supervisor, system-call validation and the timing histograms, which
the FreeRTOS and Zephyr configurations used here do not.

## 8. Faults and watchdog
See docs/TEST_PLAN.md (fault campaign: 17 cases on hardware; watchdog degraded-mode tests).

## 9. Long soak (Kestrel)
10,429,500 control periods (2.9 h) with 0 misses, 0 overruns, 0 faults, no reset; control start latency
p50 24.5 / p99 45.0 / p99.99 46.5 / max 50.5 us over all jobs; identical p50/p99 in both full hours (docs/TEST_PLAN.md section S).
