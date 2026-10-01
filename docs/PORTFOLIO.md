# Portfolio and resume wording

Every number here is backed by docs/BENCHMARKS.md, docs/TEST_PLAN.md or docs/PHASE2_RESULTS.md and the
result files they cite. Keep the qualifiers: one board, not certified, fault isolation rather than security.

## Repository tagline
Bare-metal C runtime for ARM Cortex-M4F: fixed-priority scheduling, MPU/watchdog fault handling, DMA I/O, ESP32 network fault isolation, hardware-validated.

## Portfolio description (about 150 words)
Kestrel-RT is a real-time runtime I wrote in C from the reset vector up for a 48 MHz Arm Cortex-M4F (Renesas
RA4M1): startup and linker script, a tickless fixed-priority scheduler, PendSV context switching, MPU task
isolation, a voting watchdog supervisor, crash capture, binary tracing and register-level DMA drivers, with no
heap and no vendor RTOS. I validated it on the physical board: 10,429,500 control periods with no deadline miss,
17/17 injected fault cases, interrupt storms up to 300 kHz, and a same-workload comparison against FreeRTOS,
Zephyr and a superloop (FreeRTOS had lower latency; Kestrel degraded more gracefully under overload). Phase 2
moved networking onto the board's ESP32-S3 behind a budgeted UART/DMA mailbox. Through Wi-Fi floods, 15,000
malformed frames and 11 forced ESP crashes over 10,003,440 control periods, the real-time side had zero resets and
zero critical deadline misses. Tested on one board; fault isolation, not a security boundary.

## Resume bullets

Technical (embedded, firmware, robotics controls roles):
- Wrote a zero-allocation bare-metal runtime in C for a 48 MHz Cortex-M4F from the reset vector up (tickless
  fixed-priority scheduler, PendSV switching, MPU isolation, watchdog voting, DMA UART, crash capture); validated
  on hardware with 0 deadline misses over 10.4M control periods and 17/17 injected fault cases, and benchmarked
  against FreeRTOS, Zephyr and a superloop on the same workload.
- Isolated real-time control from an ESP32-S3 network processor through a budgeted UART/DMA mailbox (sessions,
  replay rejection, rate limiting, 300 us per-job CPU budget): 0 RA4 resets and 0 critical misses across 11 forced
  ESP crashes, 15,000 malformed frames and Wi-Fi floods, at a measured +3.3 us p99 latency cost.

Compact (one-page resume):
- Built a bare-metal Cortex-M4F runtime in C (scheduler, PendSV, MPU, watchdog, DMA) and an ESP32-S3 fault-isolated
  network link; 0 deadline misses over 20M+ hardware-tested control periods, including 11 forced network-processor crashes.

## Why this matters for robotics firmware
Robot controllers put a deterministic control loop next to code that is not deterministic: networking, logging,
perception. Kestrel-RT is a small, fully inspectable instance of that problem:
- deterministic control infrastructure on an MCU, with deadline misses detected by the kernel, not inferred;
- fault handling that separates "park this task" from "reset the board", with a crash record that survives the reset;
- mixed criticality: a network processor that can flood, hang or crash without reaching the control loop;
- stress and fault-injection validation on real hardware, with failures kept and root-caused;
- honest comparison against production RTOSes, including where they win.

## Interview counterweights (say these before being asked)
- FreeRTOS had lower start latency (p50 15.9 vs 24.6 us). Kestrel spends that on per-job deadline accounting,
  64-bit tickless timekeeping and MPU retargeting.
- The Phase 2 gate G6 failed first (507 us link job against a 400 us limit), from a coarse budget check; fixed
  before the final campaign and kept in the results.
- The ESP32-S3 still controls the RA4M1's reset and SWD lines: fault isolation, not security.
- The +3.3 us Phase 2 p99 cost is constant and measured, but its cause is not isolated.
- One board. No motor was driven; the control task is a timing-representative synthetic workload.
