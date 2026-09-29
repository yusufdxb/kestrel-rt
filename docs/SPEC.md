# Kestrel-RT Engineering Specification

Status: FROZEN v1.0 (2026-09-29). Changes after this point are recorded as numbered amendments at the
end of this file, never as silent edits.

## 1. What Kestrel is

Kestrel-RT is a deliberately small, deterministic, bare-metal runtime for a resource-constrained robotic
controller. It runs on the Renesas RA4M1 (Arm Cortex-M4F, 48 MHz, 256 KB flash, 32 KB SRAM) of an Arduino
UNO R4 WiFi, starting from its own reset handler. It exists to show a fully understood runtime with
measured engineering tradeoffs, not to compete with general-purpose RTOSes.

Kestrel is NOT:

- a general-purpose RTOS, a POSIX layer, or a userspace/process kernel;
- a claim of a new scheduling algorithm;
- certified or certifiable for hard real-time or functional safety use. No certification claim is made.

## 2. Target and boundaries

| Item | Value |
|---|---|
| MCU | R7FA4M1AB3CFM (Cortex-M4F r0p1, FPU, MPU with 8 regions, DWT, 4 NVIC priority bits) |
| Core clock | 48 MHz HOCO (ICLK), PCLKA 48, PCLKB 24, PCLKC 48, PCLKD 48, FCLK 24 MHz |
| Flash available to Kestrel | 0x0000_4000 to 0x0003_FFFF (240 KB). 0x0000_0000 to 0x0000_3FFF is the Arduino bootloader plus option-setting memory and is never written |
| SRAM | 0x2000_0000 to 0x2000_7FFF (32 KB) |
| Host link | SCI9 (P109 TXD9 / P110 RXD9) to the on-board ESP32-S3 USB bridge, 460800 baud 8N1 |
| Programming | Arduino bootloader (SAM-BA, bossac) entered with the proven SWD-assisted double-tap workflow |
| Debug | SWD through the ESP32-S3 CMSIS-DAP, read-mostly; flash is never written over SWD |

The final Kestrel image must not link or call: the Arduino core (`setup`, `loop`, `Serial`, `digitalWrite`,
scheduler), Arduino FreeRTOS, Renesas FSP drivers or BSP, or Zephyr. Allowed third-party code: the Arm
CMSIS-Core headers and the SVD-generated `R7FA4M1AB.h` register header (both Apache-2.0), plus the
compiler's freestanding headers and libgcc. The FreeRTOS and Zephyr baselines are separate images under
`baselines/` and never link into Kestrel.

## 3. Runtime constraints (hard rules)

1. At most 8 tasks (`KESTREL_MAX_TASKS = 8`), defined in a static table at build time.
2. Static allocation only. No `malloc`, `free`, `new`, VLAs, or heap section. The linker script defines no
   heap; any reference to `_sbrk` fails the link.
3. Fixed-priority preemptive scheduling with unique priorities. Priorities never change at run time except
   through the documented locking protocol (section 7).
4. IPC objects are fixed-size and statically allocated. Every channel declares its full-queue policy.
5. Every periodic task has an explicit relative deadline D <= T. Deadline misses are detected by the kernel
   at the deadline instant, counted, and traced.
6. Interrupt service routines do bounded work: no loops whose bound depends on external input, no
   blocking, no formatting of strings.
7. Telemetry and tracing can never block a control path. If output cannot keep up, noncritical records are
   dropped and counted, and the counter is exported.
8. Fault records survive a reset in a storage area not touched by the bootloader, and are retrievable over
   the host link after reboot. Flash is never written at run time (no flash wear).
9. Safety-critical tasks outrank communications and logging in priority.
10. Kernel code runs privileged; application tasks run unprivileged unless explicitly marked trusted.
    Kernel data is inaccessible to unprivileged tasks (enforced by the MPU).

## 4. Initial example workload (synthetic)

| Task | Priority (higher runs first) | Period | Deadline | Criticality | Watchdog vote |
|---|---:|---:|---:|---|---|
| control | 7 | 1 ms | 1 ms | critical | yes |
| safety | 6 | 2 ms | 2 ms | critical | yes |
| sensor | 5 | 2 ms | 2 ms | critical | yes |
| comms | 3 | 10 ms | 10 ms | noncritical | no (degraded flag) |
| health | 2 | 100 ms | 100 ms | noncritical | no |
| logger | 1 | best effort | none | noncritical | no |

Priority assignment is rate-monotonic with one deliberate deviation: safety and sensor share T = 2 ms and
safety is placed above sensor (the tie is broken toward the safety function). The tasks are synthetic and
their per-job work is configurable so utilization can be swept.

## 5. Timing sources

- Timebase: GPT channel 0 (32-bit GPT320) free-running from PCLKD = 48 MHz, extended to a 64-bit logical
  count by an overflow interrupt. Resolution 20.83 ns. A read is a bounded critical section.
- Release/deadline events: compare-match A of the same counter, programmed one-shot to the earliest pending
  event (tickless).
- Measurement: the DWT cycle counter, enabled by Kestrel itself at boot (never relies on a debugger).
- Drift is measured against the host clock over the telemetry link and reported; no drift correction is
  applied.

## 6. Scheduler model

- Task states: READY, RUNNING, BLOCKED (waiting for period, event flags, mutex, or sleep), FAULTED.
- Periodic tasks are loops that call `kestrel_wait_period()` at job end.
- On a deadline miss the job keeps running to completion (no abort). A release that occurs while the
  previous job is still active is skipped and counted as an overrun, so late jobs never accumulate a backlog.
- Context switching uses PendSV at the lowest priority; tasks run on PSP; the kernel and ISRs run on MSP.

## 7. IPC

| Primitive | Semantics | Full policy |
|---|---|---|
| SPSC queue | lock-free single producer / single consumer, power-of-two capacity | per channel: REJECT (return error, count) or OVERWRITE_OLDEST |
| Mailbox | latest-value, sequence-locked | always overwrite (by definition) |
| Event flags | 32-bit flag group, wait any/all with timeout | n/a |
| Mutex | immediate priority ceiling protocol (after the inversion demo, see TEST_PLAN) | n/a |

Safety-relevant channels use REJECT and raise a counted, traced event; they never silently drop.

## 8. Fault handling and supervision

- HardFault, MemManage, BusFault, UsageFault, and NMI are handled by Kestrel. Each captures the stacked
  frame (R0-R3, R12, LR, PC, xPSR), CFSR, HFSR, MMFAR, BFAR, SHCSR, ICSR, current task id, timestamp, stack
  pointer and stack high-water mark into a CRC-protected crash record.
- A fault in an unprivileged task marks that task FAULTED and the policy for that task decides: critical task
  fault -> record and reset; noncritical task fault -> record, suspend the task, keep running.
- Watchdog: the RA4M1 WDT in register-start mode. Only the supervisor feeds it. The supervisor runs in the
  kernel timer path and feeds only when every critical task completed at least one job in its window.
  Noncritical task stalls set a degraded flag and do not cause a reset.

## 9. Telemetry and tracing

- Trace: 8-byte binary records `{u32 timestamp, u16 event, u16 data}` in a static ring. Overflow drops the
  newest record and increments a counter. No strings on real-time paths.
- Transport: a DMA channel feeds SCI9 transmit from a static byte ring. Frames carry magic, version, type,
  sequence, length, payload, CRC-16.

## 10. Verification bar (Phase 1 gate)

Phase 2 (ESP32-S3 split architecture) starts only after all of the following are met on the physical board:
stable startup; scheduler; context switch; preemption; static IPC; fault handlers; stack protection;
watchdog; trace recorder; nonblocking comms; at least one peripheral workload; at least 10,000,000 control
periods without unexplained failure; overload campaign; fault campaign; FreeRTOS comparison.

## 11. Claims policy

No metric is reported unless it was measured on the physical RA4M1 in this repository with the raw data
hash committed. Sufficient schedulability bounds are never presented as necessary conditions. "MISRA
compliant" is never claimed; MISRA-inspired rules are listed in `docs/ARCHITECTURE.md`.

## Amendments

(none yet)
