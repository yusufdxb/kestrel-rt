# Changelog

## phase2-go-v1.0 (2026-09-30)
Publication release. No runtime behavior changed relative to the validated Phase 2 images (RA4M1
`kestrel.bin` sha256 `14c04cb3...8cb` is rebuilt byte for byte; ESP32-S3 `KestrelBridge.bin`
`f1a96a1c...bdd4` is rebuilt identical in code and data, differing only in the embedded ELF hash and
image digest). Phase 2 C sources were reformatted to pass the clang-format gate, which had been
misreporting deviations as clean (whitespace only; images unchanged).
- Phase 2: split-criticality RA4M1 + ESP32-S3. SCI1 DMA link, KX protocol with sessions, sequence window,
  replay rejection and rate limiting, budgeted link task, ESP32-S3 bridge fork, recovery gate and stock
  restore. Frozen gates G1-G9 all pass on the final images; G6 failed once and was fixed. 10,003,440-period
  soak with 11 forced ESP crashes: 0 RA4 resets, 0 critical misses, 0 desyncs.
- Publication: the ESP32-S3 sketch ships as an ed script against the pinned upstream commit instead of
  a modified copy (the upstream has no license); licensing inventory, publication audit, design-decision and
  engineering-log docs, rewritten README, `scripts/demo.sh`; board MAC, local Wi-Fi name, local paths and
  internal project references removed from the current tree.

## phase1-go-2026-09-30 (2026-09-30, commit 1318e51)
Single-processor runtime. Own startup, tickless fixed-priority scheduler, PendSV switching, MPU, fault
capture, watchdog supervisor, DMA UART, flight recorder. Benchmarked against a superloop, FreeRTOS 11.1.0
and Zephyr 4.2.0 on the same workload; 17/17 fault cases; 10,429,500-period soak with 0 misses.
