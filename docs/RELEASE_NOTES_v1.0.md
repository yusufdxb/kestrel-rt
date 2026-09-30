# Kestrel-RT phase2-go-v1.0

A zero-allocation bare-metal real-time runtime in C for the ARM Cortex-M4F (Renesas RA4M1, 48 MHz),
extended into a split-criticality design with the board's ESP32-S3 as an untrusted network processor.
Evaluated on one Arduino UNO R4 WiFi.

## Results on hardware
- Phase 1: 10,429,500 control periods, 0 deadline misses; 17/17 fault cases; 0 misses under a 300 kHz
  interrupt storm; kernel 12,291 B flash / 5,967 B RAM. FreeRTOS 11.1.0 had lower start latency
  (p50 15.9 vs 24.6 us); Kestrel degraded more gracefully under 7x overload.
- Phase 2: 10,003,440 control periods with 11 forced ESP32-S3 crashes: 0 RA4 resets, 0 critical misses,
  0 protocol desyncs. 15,000 malformed frames and 32 stale replays rejected, 83,711-frame flood bounded and
  counted, 5.9 Mbit/s Wi-Fi flood without effect on control timing. Cost: a fixed +3.3 us at control p99.

## Limits
Not certified (RTOS, MISRA, SIL), not formally verified, not a security boundary: the ESP32-S3 keeps
physical RESET and SWD control of the RA4M1. One board. No real actuator was driven. docs/LIMITATIONS.md.

## Contents
Source and documentation only. No firmware binaries are attached. The ESP32-S3 fork is rebuilt from
Arduino's upstream bridge 0.5.2 plus an ed script (esp/README.md, docs/LICENSING.md).

Start with README.md, then docs/PHASE2_RESULTS.md and docs/ENGINEERING_LOG.md.
