# Portfolio and resume wording

Every number below is backed by docs/BENCHMARKS.md, docs/TEST_PLAN.md or docs/PHASE2_RESULTS.md.
Keep the qualifiers: one board, not certified, fault isolation rather than security.

## Long version (portfolio page, cover letter)
Designed and implemented Kestrel-RT, a zero-allocation bare-metal runtime in C for a 48 MHz ARM Cortex-M4F.
Built custom startup, fixed-priority preemptive scheduling, PendSV context switching, MPU isolation, watchdog
supervision, DMA I/O, crash capture and binary tracing. Validated it over 10.4M control periods and
fault/overload campaigns, then extended it into a split-criticality RA4M1/ESP32-S3 system where the
real-time domain maintained zero deadline misses through network floods, malformed traffic, ESP crashes and
reboot loops.

## Resume bullets
- Built a zero-allocation bare-metal RTOS in C for a Cortex-M4F (PendSV switching, MPU isolation, watchdog
  voting, DMA I/O, crash capture): 0 deadline misses over 10.4M control periods and 17/17 hardware fault cases.
- Split real-time control from an untrusted ESP32-S3 network processor over a budgeted UART/DMA protocol:
  0 RA4 resets and 0 missed deadlines through 11 forced network-processor crashes and 15,000 malformed frames.

## One line
Bare-metal Cortex-M4F runtime in C, benchmarked against FreeRTOS and Zephyr on hardware and extended into a
fault-isolated dual-processor design.

## Interview talking points (with the honest counterweight)
- FreeRTOS had lower start latency (p50 15.9 vs 24.6 us). Kestrel spends that on per-job deadline accounting,
  MPU retargeting and fault capture, and in exchange degrades gracefully under 7x overload.
- The Phase 2 gate G6 failed first (507 us link job against a 400 us limit). The root cause was a coarse
  budget check, found and fixed before the final campaign; the failure is kept in the results.
- The ESP32-S3 still controls the RA4M1's reset and SWD lines, so the project claims fault isolation, not
  security.
- The +3.3 us Phase 2 p99 cost is measured and constant, but its cause is not isolated.
