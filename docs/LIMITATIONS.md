# Limitations

## Scope (by design)
- Not a general-purpose RTOS: at most 8 tasks, static task set, unique priorities, no dynamic creation,
  no heap, no filesystem, no shell, no networking on the RA4M1, no POSIX.
- No certification claim (hard real-time or functional safety). MISRA principles are applied selectively;
  no MISRA compliance claim.
- Single core; the IPC primitives rely on it (the priority-ceiling mutex in particular).

## Measured weaknesses
- **Release-to-start latency is higher than FreeRTOS** on this hardware (control p50 24.6 us vs 15.9 us, p99
  44.9 vs 18.7 us). The kernel timer ISR does per-job accounting and 64-bit tickless timekeeping; the
  context-switch path is ~2.7 us slower per signal-to-wake than FreeRTOS notifications.
- **Tail latency from the kernel timer ISR**: it runs above every device interrupt by design; when it lands
  inside a context switch it adds up to ~26 us (0.1% of ctxbench samples).
- **Kernel footprint** (12.3 KB flash, 6.0 KB RAM) is larger than the FreeRTOS kernel in the comparison
  configuration (2.8 KB / 0.9 KB), because it includes fault capture, crash log, MPU policy, supervisor and
  instrumentation.
- **Overload recovery**: when the CPU is overloaded the lower-priority command/telemetry task starves, so the
  host cannot reduce the load remotely; critical tasks keep voting, so the watchdog does not reset either.
- Unexplained: at 300 kHz interrupt storm, 1.3% of kernel-timer entries exceeded 5.3 us (max 12.8 us) although
  the storm has a lower priority.

## Platform constraints (UNO R4 WiFi)
- The host link goes through the ESP32-S3 USB bridge. Observed on this board: its CDC data path stalls when the
  host stops reading and sometimes around RA4 resets (especially SWD-driven ones); repeated hub-port cycles
  left it unable to enumerate (physical replug needed, twice). The RA4M1 kept running in every case.
- Automatic bootloader entry through the bridge is unreliable on this board; uploads use an SWD-assisted
  double-tap entry. Flash is never written over SWD.
- Warm resets pass through the Arduino bootloader's 500 ms double-tap wait, which dominates recovery time.
- RSTSR0.PORF is not reliably set when Kestrel runs after a power-up; cold boot is inferred from lost VBATT
  backup-register contents.
- Writing the crash record to the VBATT backup registers costs about 9 us per byte (1.1 ms per record).
- Code runs from flash at one wait state (48 MHz); straight-line kernel code executes at a CPI of about 3.

## Measurement limits
- Start latency is task-observed (first instruction of the job); Kestrel's execution time includes its own
  job-end system call; exact definitions per system in docs/BENCHMARKS.md.
- Zephyr is measured in its own SysTick cycle domain with a one-time tick-edge calibration (few-cycle error).
- On-target histograms report bin upper edges; values above a histogram's range are reported as maxima only.
- The benchmark image keeps experiment hooks reachable from the host link (MPU off, flash-cache off, storm,
  fault injection); a production image would compile them out.

## Not implemented
CAN (controller present, no transceiver on the board; loopback driver not written), native USB, low-power modes,
DTC, and Phase 2 (ESP32-S3 split architecture; docs/SPLIT_ARCHITECTURE.md).
