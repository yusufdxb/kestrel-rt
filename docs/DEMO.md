# Live demo

```bash
scripts/demo.sh            # about 1 minute: runtime state, timing capture, contained MPU fault, recovery
KX_HOME_WIFI=<connection> scripts/demo.sh --esp   # about 2 minutes: adds the ESP32-S3 crash (Phase 2 images)
scripts/demo_record.sh     # the --esp run for screen recording; transcript in build/demo_transcript.txt
```

Requirements: the UNO R4 WiFi on USB, the Phase 2 RA4M1 image (the script builds it, and flashes only if the
board's flash differs), and for `--esp` the Kestrel ESP32-S3 fork plus a host Wi-Fi card that can join its SoftAP.

**What is live and what is not.** Every line marked `[live]` is read from the board during the run: kernel
status frames and per-job records over USB, a crash record, RA4 heartbeats forwarded over Wi-Fi by the ESP32-S3,
and the RA4's link counters. The closing `VALIDATED CAMPAIGNS` block is read from the recorded result files
named on each line. The demo does not reproduce the 10M-period soaks; it shows the same behavior once.

## Two-minute recording script

| Time | Show | Say |
|---|---|---|
| 0:00 | README architecture figures | One Cortex-M4F runs the real-time domain on Kestrel; the ESP32-S3 is an untrusted network processor behind a bounded UART/DMA link. |
| 0:20 | `board` and `runtime` stages | Own startup from the reset vector, no vendor RTOS. Six tasks plus the link task, with deadline-miss counters kept by the kernel. |
| 0:40 | `timing` stage | Per-job records streamed from the kernel: control start latency p50 ~25 us, p99 ~48 us, 0 misses. |
| 1:00 | `fault` stage | A NULL write in an unprivileged task: the MPU traps it, the crash record is captured in ~45 us, the task is parked, control keeps running with 0 misses. |
| 1:20 | `Phase 2` stages | Heartbeats over Wi-Fi, then a forced ESP panic: telemetry stops, the RA4 keeps its boot epoch, control jobs advance about 1 per ms, a new link session starts. |
| 1:40 | USB counters after the ESP returns | RA4 boot count unchanged, 0 critical misses, the outage logged on the RA4 itself. |
| 2:00 | `VALIDATED CAMPAIGNS` block | The recorded campaigns: 10.4M and 10.0M control periods, 17/17 fault cases, 11 forced ESP crashes with 0 RA4 resets. The cost is +3.3 us at p99. FreeRTOS had lower start latency. |

## Transcript of a run (2026-09-30; lines marked `...` are elided, nothing else changed)

```text
KESTREL-RT  bare-metal runtime, Renesas RA4M1 (Arm Cortex-M4F, 48 MHz) + ESP32-S3 network domain

-- board -------------------------------------------------------------
  [live] USB 2341:1002 Arduino SA UNO WiFi R4 CMSIS-DAP
  build  APP=phase2  kestrel.bin 14c04cb3f6951788...
  [live] flash at 0x4000 matches the build byte for byte (read-only SWD)

-- runtime on the RA4M1 (USB) ----------------------------------------
  [live] boot #7, last reset: software+warm, uptime 1262.7 s, faults 0, watchdog warnings 0
  [live] control  jobs 1,262,741  misses 0  stack  264 B  waiting
  ...  (safety, sensor, comms, health, logger, link)

-- timing: 8 s of per-job records streamed from the kernel -----------
           task      jobs  misses  start p50 / p99 / max (us)   CPU
  [live] control    9,340       0    24.96 /  47.88 /  50.71   18.2%
  ...

-- fault: NULL write in the unprivileged logger task -----------------
  [live] MPU trapped it: MemManage DACCVIOL MMARVALID, fault address 0x10, PC 0x8302 (inject_check)
  [live] crash record captured in 2158 cycles (45 us), read back over USB
  [live] logger parked FAULTED; board not reset (boot #7 -> #7)
  [live] control kept running: +2000 jobs, +0 misses

-- designed recovery: host-requested software reset restarts the parked task
  [live] boot #8, reset: software+warm, logger ready

-- Phase 2: ESP32-S3 link online -------------------------------------
  [live] RA4 link: session 46789, 36 frames accepted, 1 session(s) since boot, degraded 0
  [live] ESP32-S3: boot #27, Wi-Fi client(s) 1, PING RTT avg 3570 us, heap 184,396 B, 0 resyncs
  [live] RA4 heartbeats over Wi-Fi: 25 in 2.5 s, RA4 epoch 585065, control jobs 6,501, misses 0

-- force an ESP32-S3 panic (NULL write on the ESP) -------------------
  [live] ESP reply: ok crash
  [live] ESP gone: no telemetry for 2.2 s (ESP reboot, Wi-Fi rejoin, new session)
  [live] telemetry resumed: RA4 epoch 585065 (same RA4 boot), link session 46789 -> 13097
  [live] RA4 control ran through it: +2,300 jobs in 2.3 s (1 ms period), misses 0 -> 0

-- RA4 kernel counters over USB after the ESP re-enumerates ----------
  [live] RA4 boot #8 -> #8, critical misses +0, sessions 1 -> 2
  [live] RA4 outage log, latest entry: link lost for 405 ms

-- VALIDATED CAMPAIGNS (recorded results, not this run) --------------
  Phase 1 soak    10,429,500 control periods (2.9 h), critical misses 0            results/soak_10M.json
  Fault campaign  17/17 cases pass on hardware                       results/fault_campaign_run5_final.json
  Phase 2 soak    10,003,440 control periods, 11 forced ESP crashes, RA4 resets 0, critical misses 0   results/phase2/soak_p2.json
  Known cost      +3.3 us control start p99 with the link task present; FreeRTOS 11.1.0 had lower
                  start latency than Kestrel (docs/BENCHMARKS.md, docs/LIMITATIONS.md)

live demo OK

real	0m59.190s
```

Reading the Phase 2 lines: the 2.2 s without telemetry covers the ESP reboot and the host rejoining its
SoftAP; the RA4's own outage log measured the UART link down for 405 ms. The "boot #8" comes from the
software reset in the recovery stage, before the ESP crash; the ESP crash itself changed nothing on the RA4.
