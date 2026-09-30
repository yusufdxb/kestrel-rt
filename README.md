# Kestrel-RT

A small deterministic runtime in C for a resource-constrained robotic controller, written from the reset
vector up for the Renesas RA4M1 (Arm Cortex-M4F, 48 MHz, 256 KB flash, 32 KB SRAM) on an Arduino UNO R4 WiFi,
and measured on the physical board against a superloop, FreeRTOS 11.1.0 and Zephyr 4.2.0 running the same
workload.

It is not a new RTOS algorithm and not a certified kernel. It is a fully understood runtime (about 4,200
non-blank lines of C and 125 of assembly for startup, kernel, safety, tracing and drivers) with its engineering
tradeoffs measured.

![Architecture](docs/figures/architecture.svg)

## What is in it
- **Own startup**: vector table at 0x4000 behind the untouched Arduino bootloader, reset handler, `.data`/`.bss`
  init, FPU, clocks (HOCO 48 MHz), flash cache. No Arduino core, no FSP, no Zephyr in the runtime; only the
  CMSIS-Core and SVD register headers are used.
- **Scheduler**: fixed-priority preemptive, O(1) ready set, tickless release timing on a 64-bit timebase
  (GPT320, 20.8 ns), deadline misses detected at the deadline instant, skip-release overrun policy.
- **Context switch**: PendSV saving R4-R11, EXC_RETURN and CONTROL, plus S16-S31 only for tasks with FP context
  ([docs/CONTEXT_SWITCH.md](docs/CONTEXT_SWITCH.md)).
- **Isolation**: tasks run unprivileged; 8 MPU regions (kernel data private, per-task stack region + guard,
  NULL page trap, SRAM never executable); validated system calls.
- **IPC without allocation**: lock-free SPSC queues, sequence-locked mailboxes, event flags, and a priority-ceiling
  mutex (plus a plain one to demonstrate inversion).
- **Fault handling**: HardFault/MemManage/BusFault/UsageFault capture (stacked frame, CFSR/HFSR/MMFAR/BFAR/...)
  into a CRC-protected record in the VBATT backup registers, retrievable after reboot; containment policy per task.
- **Watchdog supervision**: critical tasks vote, one supervisor feeds the WDT; noncritical stalls only degrade.
- **I/O**: register-level drivers; UART TX and RX both via DMA; ADC via a timer-paced ISR into an SPSC queue.
- **Flight recorder**: binary 8-byte events and 12-byte per-job records, streaming or armed/triggered/frozen
  capture, host decoder and timeline tool.

## Results (physical RA4M1; details and raw-data hashes in [docs/BENCHMARKS.md](docs/BENCHMARKS.md))

![Start latency CDF](docs/figures/start_latency_cdf.svg)

| | Kestrel | FreeRTOS 11.1.0 | Zephyr 4.2.0 | Superloop |
|---|---:|---:|---:|---:|
| control start latency p50 / p99 / max (us) | 24.6 / 44.9 / 49.0 | 15.9 / 18.7 / 22.3 | 32.7 / 44.6 / 54.7 | 32.0 / 54.7 / 62.1 |
| deadline misses, 30 s nominal load | 0 | 0 | 0 | 0 |
| signal-to-wake context switch p50 (us) | 13.0 | 10.3 | - | - |
| image flash / RAM (bytes) | 23,046 / 20,569 | 11,074 / 15,659 | 19,763 / 20,150 | 8,158 / 7,094 |
| at 7x overload | control at half rate, safety/sensor/comms 0 misses | control misses every job, all other tasks starved | not measured | every task misses |

- FreeRTOS is faster and smaller on the latency path; Kestrel spends that difference on per-job deadline and
  latency accounting, MPU retargeting and system-call validation, and gains deterministic overload behavior,
  fault containment and a flight recorder.
- Fault campaign (17 cases on hardware): contained faults (NULL, unmapped, kernel-write, stack overflow, XN,
  UDF, div0, assert) caused 0 control deadline misses; critical-task faults and stalls reset with the right
  crash record after reboot.
- Priority inversion: 5.17 ms blocking with a plain mutex, 0.91 ms with the priority-ceiling mutex.
- Telemetry backpressure: a 5 s TX stall and a 17 s host read stall left every deadline intact; dropped records
  are counted exactly.

- Soak: 10,429,500 control periods (2.9 h) with 0 misses, 0 overruns, 0 faults; control start latency max 50.5 us.
- Final fault campaign: 17/17 cases pass on the final firmware.

Known weaknesses: [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## Phase 2: split criticality with the board's ESP32-S3 (GO, 2026-09-30)
The ESP32-S3 (Wi-Fi, a fork of the official Arduino USB bridge that keeps CMSIS-DAP and the serial bridge)
talks to Kestrel over a 1 Mbaud DMA-backed mailbox with a sequenced, CRC-checked protocol; the RA4M1 treats
it as untrusted and serves it from a budgeted priority-4 task. Measured on the board
([docs/PHASE2_RESULTS.md](docs/PHASE2_RESULTS.md)):
- Wi-Fi flood, ESP CPU saturation, heap exhaustion, 15,000 malformed frames, a UART-saturating mailbox flood,
  a held-low line, reboot loops, panics and watchdog hangs: **0 RA4 critical deadline misses, 0 RA4 resets**;
  control start p99 47.88 us in every case (44.54 us with the link compiled out: a fixed 3.3 us cost).
- Soak: 10,003,440 control periods with 11 forced ESP crashes: 0 misses, 0 RA4 resets, 0 desyncs; each ESP
  crash is a ~0.4 s link outage followed by a clean new session.
- Fault isolation / trust-boundary design, not security: the ESP still owns the RA4 RESET line and SWD.
Docs: [ARCHITECTURE](docs/PHASE2_ARCHITECTURE.md) · [INTERCONNECT](docs/PHASE2_INTERCONNECT.md) ·
[PROTOCOL](docs/PHASE2_PROTOCOL.md) · [FAULT_MODEL](docs/PHASE2_FAULT_MODEL.md) · [RECOVERY](docs/PHASE2_RECOVERY.md) ·
[RESULTS](docs/PHASE2_RESULTS.md)

## Build, flash, measure
```bash
# toolchain: Arm GNU 13.3.rel1 in ~/.local/opt (Makefile TOOLCHAIN=...)
make APP=benchmark                  # build/benchmark/kestrel.{elf,bin,map,lst}; image gate runs at link
make flash APP=benchmark            # SWD-assisted bootloader entry + bossac; bootloader verified before/after
python3 host/experiment.py run1 --secs 30      # per-task latency percentiles, histograms, CPU share
make host-test && scripts/lint.sh               # host tests (ASan/UBSan/TSan, fuzz) + cppcheck/clang-format
scripts/fetch_baselines.sh && make -f baselines/baseline.mk BASE=freertos   # baselines (superloop, freertos)
scripts/zephyr_image.sh                         # Zephyr baseline (needs a west workspace)
```

## Documents
[SPEC](docs/SPEC.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) · [SCHEDULER](docs/SCHEDULER.md) ·
[CONTEXT_SWITCH](docs/CONTEXT_SWITCH.md) · [MEMORY](docs/MEMORY.md) · [FAULT_HANDLING](docs/FAULT_HANDLING.md) ·
[DRIVERS](docs/DRIVERS.md) · [TEST_PLAN](docs/TEST_PLAN.md) · [BENCHMARKS](docs/BENCHMARKS.md) ·
[LIMITATIONS](docs/LIMITATIONS.md) · [SPLIT_ARCHITECTURE](docs/SPLIT_ARCHITECTURE.md)

## License
MIT (see LICENSE). Vendored register headers: Apache-2.0 (third_party/README.md).
