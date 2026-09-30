# Split-criticality architecture (Phase 2) - NOT STARTED

Phase 1 passed its gate on 2026-09-30 (docs/SPEC.md section 10: soak 10.43 M periods, fault campaign 17/17,
overload and FreeRTOS comparisons complete). No ESP32-S3 firmware has been written or flashed: replacing the
bridge firmware is an irreversible change to this board's only USB and SWD path and needs an explicit decision.

## Intent
RA4M1 = deterministic, trusted domain (scheduler, timing, safety state, control workload, watchdog, critical
logs). ESP32-S3 = communications, untrusted domain (Wi-Fi, network stack, remote telemetry, configuration
transport). The ESP32 never changes critical RA4 state asynchronously: every command crosses a validated mailbox
protocol and is applied at a defined safe point.

## Hardware path (from the board files; not yet verified on this board)
- RA4M1 SCI1 on P501 (TXD) / P502 (RXD) is wired to the ESP32-S3 ("TX/RX WIFI" in the Arduino variant), separate
  from the SCI9 USB bridge link.

## Protocol (reuses trace/frame.c)
magic, version, type, sequence, length, payload, CRC-16. RA4 receiver rules: bounded payload (480 B), no
allocation, timeout, sequence validation, rate limit, discard invalid frames, configuration applied only at
safe points by the task that owns it.

## Risk gate before touching the ESP32
Programming the ESP32-S3 replaces the Arduino bridge firmware, which is also this board's only USB and SWD
path. Required first: back up the exact bridge firmware (version 0.5.2 on this board), document and test the
restore procedure with a second recovery path, and confirm the RA4 can still be programmed and debugged.

## Hypothesis to test
Network/communications failures on the ESP32 (Wi-Fi flood, reconnect loop, CPU saturation, malformed traffic,
reboot, crash, garbage on the interprocessor link, long stalls) cause no deadline failures in the RA4 domain.
Phase 1 already gives a partial, bridge-level datapoint: when the host stopped reading and the stock ESP32
bridge stalled, the RA4 kept every deadline (docs/BENCHMARKS.md section 5).
