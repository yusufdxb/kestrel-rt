# Test plan and results

Status labels: **HW** = executed on the physical RA4M1 with evidence in `results/`; **host** = executed on the
Linux host; **pending** = not yet executed. Nothing is marked HW without a results file.

## H. Host tests (portable components) - host
`make host-test` (tests/Makefile): ASan + UBSan for all suites, TSan for the concurrent SPSC test.
| Suite | What it proves |
|---|---|
| test_spsc | FIFO order, REJECT policy + counter, high-water, index wrap across 2^32 (starts at 0xFFFFFFF0) |
| test_spsc_tsan | 2,000,000 elements through a 16-slot queue between two threads, in order, no data race reports |
| test_mailbox | latest-value semantics, sequence numbering, bounded retries (KMB_BUSY instead of a torn read) |
| test_frame | round trip at every length 0..480, single-bit corruption at every position, header rejects, resync window, 20,000-round deterministic fuzz (garbage + mutated frames): no crash, no frame with a wrong CRC delivered |
| test_crc | CRC-16/CCITT-FALSE and CRC-32 check values |
| test_timebase | 64-bit extension of the 32-bit counter across the wrap with a late overflow ISR: exact and monotonic |
| test_khist | histogram binning and overflow |
Static analysis: `scripts/lint.sh` (cppcheck warning/performance/portability, clang-format): clean.
Deliberate faults in the injector carry justified inline suppressions.

## F. Fault injection - HW (`host/fault_campaign.py`)
17 cases. Contained cases target noncritical unprivileged tasks (health, logger): the task must be parked
FAULTED, no reset, control must keep every deadline, and the crash record must be retrievable at once.
Reset cases target critical or privileged tasks: the board must reset, and the record must be retrieved
after reboot with the right class and reset cause.

| Case | Expected | Observed (runs 2 and 3) |
|---|---|---|
| NULL read, NULL write (health/logger) | MemManage DACCVIOL, MMFAR 0x0 / 0x10, contained | as expected, control misses 0 |
| illegal instruction, divide by zero | UsageFault UNDEFINSTR / DIVBYZERO, contained | as expected |
| unprivileged read of unmapped 0x70000000 | MemManage (no MPU region for unprivileged) | MMFAR 0x70000000 |
| unprivileged write into kernel .bss | MemManage, kernel data untouched | MMFAR 0x20000898 (kernel .bss) |
| stack overflow (recursion) | MemManage via 32 B guard | DACCVIOL + MSTKERR, fault address in the guard block |
| jump into SRAM | MemManage IACCVIOL (SRAM is XN) | as expected |
| assert, task entry returns | software records (Assert, TaskExit), contained | as expected |
| NULL read in control, div0 in sensor, stack overflow in safety, assert in control | record + software reset | as expected; reset cause software; host-observed recovery 0.60 s |
| read of 0x70000000 in the privileged comms task | BusFault PRECISERR, BFAR exact, reset | BFAR 0x70000000 |
| control hangs, sensor hangs | supervisor stall record (PC in the hang loop), WDT reset | reset cause WDT; recovery 0.90 s |

- Full run 2: 16/17 (hang_control: the injection command never reached the board); isolated reruns of that
  case: 3/3. Full run 3: 16/17 (assert_critical: same symptom). Root cause of the lost commands found later:
  SCI9 receive overruns while the kernel timer ISR ran longer than one character time; fixed by DMA
  reception (300/300 commands acknowledged afterwards, 0 receive errors). Run 4 with the fix: the 4 cases it
  reached passed, then the ESP32 USB bridge stopped enumerating (after repeated hub-port cycles).
- **Run 5 (final firmware, 2026-09-30): 17/17 PASS** in one full run, no bridge recovery needed
  (`results/fault_campaign_run5_final.json`). Host-observed recovery 0.755 s (fault reset), 1.00 s (WDT).
- Capture latency (fault entry to record ready, before the backup-register write): 1,846-2,174 cycles
  (38-45 us); 391-484 cycles for stacking faults; 3,875 cycles for the privileged bus fault.
- Contained faults do not delay control: the first version wrote the record to the VBATT backup registers
  inside the fault handler (about 9 us per byte, 1.1 ms per record) and caused a control miss (caught with the
  flight recorder); records of contained faults are now persisted by the comms task.
- Recovery time is dominated by the Arduino bootloader's 500 ms double-tap wait on non-power-on resets.

## W. Watchdog policy - HW (`host/wd_degraded.py`, `results/wd_degraded.json`)
| Hang | Expected | Observed (8 s, counters over SWD, no halt) |
|---|---|---|
| comms | degraded flag, no reset | degraded 0x38 (comms + starved health and logger), no reset, critical tasks 0 misses |
| health | degraded, no reset | 0x30, no reset, 0 misses |
| logger | degraded, no reset | 0x20, no reset, 0 misses |
| control, sensor | reset | see section F |
Debugger note: halting the core while the watchdog runs is itself detected as a stall (the timebase keeps
counting), and the supervisor resets the board. All diagnostics therefore use non-halting SWD reads.

## M. MPU - HW
Covered by section F: kernel-write blocked, NULL page trapped, SRAM not executable, stack guard, unprivileged
access to unmapped memory denied. MPU cost per switch pair: about 0.3 us (docs/BENCHMARKS.md section 2).

## P. Priority inversion - HW (`apps/inversion`, `results/inversion.json`)
Plain mutex 5,173 us mean blocking, ceiling mutex 909 us. See docs/BENCHMARKS.md section 6.

## O. Overload, interrupt storm, telemetry stall - HW
docs/BENCHMARKS.md sections 3, 4 and 5.

## R. Timer rollover - HW + host
- Host: `test_timebase` sweeps the wrap with a late overflow ISR.
- HW: a 310 s run crossed the 32-bit counter wrap three times (89.5, 179.0, 268.4 s) with 310,061 control jobs,
  0 misses, 0 overruns, 0 watchdog warnings. The accelerated preload hook (`kt_test_preload`) is not exercised.

## Q. Queue overflow - host + HW
REJECT policy and counters: host tests. On hardware, the trace/job rings overflowed on purpose in the TX stall
test (10,380 records dropped and counted, equal to the host's gap count).

## X. Malformed host packets - host + HW
Fuzzed decoder (host). On hardware, receive errors before the DMA fix corrupted command frames; all were
rejected by CRC and counted (`cmd_frames_bad`), never executed.

## S. Soak - HW, PASS (`results/soak_10M.json`, raw sha256 3a15b816...)
2026-09-30, final benchmark image, nominal load, 10,429.5 s:
- control 10,429,500 jobs, 0 misses, 0 overruns; safety and sensor 5,214,750 jobs each, 0 misses; comms
  1,042,950 and health 104,295 jobs, 0 misses; 0 faults, 0 watchdog warnings, no reset (boot count unchanged).
- host received every control record (0 sequence gaps), 0 link outages.
- control start latency over 10.43 M jobs (0.5 us bins): p50 24.5, p95 43.0, p99 45.0, p99.99 46.5, max 50.5 us;
  p50/p99 identical in both full one-hour buckets (the final 0.9 h is only in the total): no drift observed.
