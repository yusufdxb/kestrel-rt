# Engineering log: what failed, why, and what now guards it

Each entry: **failure -> root cause -> fix -> regression test**. Nothing here was removed from the
results; the original runs stay in `results/` next to the reruns. "Not fixed" means the weakness is
documented in docs/LIMITATIONS.md instead.

## Phase 1 (single processor)

| # | Failure | Root cause | Fix | Regression test / evidence |
|---|---|---|---|---|
| 1 | High-priority task blocked 5,173 us behind a medium-priority burst | unbounded priority inversion with a plain mutex (demonstrated on purpose) | immediate priority-ceiling mutex as the production protocol | `apps/inversion`: ceiling blocking 909 us (`results/inversion.json`) |
| 2 | Fault-campaign runs 2 and 3 each lost one injection command (16/17) | SCI9 receive overruns: the kernel timer ISR sometimes ran longer than one character time, so the per-byte RX interrupt dropped bytes | UART reception through a DMA repeat-mode ring | 300/300 commands acknowledged, 0 receive errors; run 5: 17/17 in one run (`results/fault_campaign_run5_final.json`) |
| 3 | A **contained** fault (in the unprivileged logger) caused a control deadline miss | the fault handler wrote the crash record to the VBATT backup registers in place, about 9 us per byte (1.1 ms per record) | contained faults keep the record in RAM; the comms task persists it later | contained cases: 0 control misses in every campaign run; found with the flight recorder |
| 4 | The USB bridge's CDC path went silent after SWD resets between cases, and stopped enumerating after repeated hub-port cycles (run 4) | ESP32-S3 bridge behavior on this board around RA4 resets | software reset command between cases; bridge recovery rate-limited to one port cycle per 10 min (`host/bridge.py`) | run 5 needed no bridge recovery; platform limitation documented |
| 5 | Telemetry backpressure: a stalled host could block the target | a design risk found by the stall tests, not a bug in the shipped code | drop-newest rings with exact counters; TX never blocks a task | 5 s TX stall: 0 misses, 10,380 drops counted = host gap count; 16.9 s host read stall: 0 misses |
| 6 | Past ~5.5x overload the command/telemetry task starves and the host can no longer reduce the load; the watchdog does not step in because critical tasks still vote | fixed priorities do exactly this; the overload response was designed for the critical tasks only | **not fixed**; skip-release policy keeps the critical tasks running; documented | overload sweep (`results/sweep_*_swd.json`) |
| 7 | Context-switch tail: 0.1% of switches up to ~40 us | the kernel timer ISR runs above every device interrupt and can land inside the measured switch | **accepted** tradeoff (release latency first); identified with ISR entry/exit tracing | `host/ctxbench.py`, docs/BENCHMARKS.md section 2 |
| 8 | At a 300 kHz interrupt storm, 1.3% of timer entries exceeded 5.3 us (max 12.8 us) | **not explained** (the storm has lower priority) | none | recorded in docs/LIMITATIONS.md |

## Phase 2 (RA4M1 + ESP32-S3)

| # | Failure | Root cause | Fix | Regression test / evidence |
|---|---|---|---|---|
| 9 | **About 1.17 million receive-error interrupts** on the RA4 while the ESP32-S3 sat in ROM download mode (no deadline missed) | the ESP's TX line sat low; every break/framing error raised an RA4 interrupt | receive-error storm guard: 32 errors per window -> receiver muted 20 ms | case F12 (line held low 15 s): 19,200 errors, 600 mutes, 0 misses, link resumed |
| 10 | RA4 replies (PONG, ACK, FAULT) never reached the ESP | the TX buffer was reset before the periodic emitter ran, discarding queued replies | double-buffered TX | PING/PONG round trips and CONFIG ACKs in every later campaign run (ESP counters) |
| 11 | **Gate G6 failed in campaign 1**: under the mailbox flood the link job reached 493 us wall / 507 us execution (limit 400 us); critical tasks unaffected | the RX time budget was checked only between 64-byte chunks, and not before TX emission | time checked every 16 bytes; 200 us RX budget | campaign 2 and final: 312 to 314 us; soak worst job 313.8 us |
| 12 | **Permanent protocol desync** after 15 s of ESP silence (campaign 1, F12) | the ESP consumed sequence numbers while not transmitting, so its frames jumped past the 64-frame window; it kept receiving RA4 heartbeats and never started a new session | a sequence number is consumed only on send; the ESP starts a new session when the RA4's acknowledged sequence stops advancing for 500 ms | final F12 and soak: 0 resyncs needed, 0 desyncs |
| 13 | The first outage-log entry was bogus | the boot period before the first session was not accounted | the boot interval is logged as a real outage | every final-campaign outage log |
| 14 | An extra HELLO at startup | `hs_sent` not set in the handshake start | set at start | one new session per ESP boot in the final campaign (RA4 `sessions` counter) |
| 15 | F11 in campaign 2 only tested pre-HELLO replays | a test bug cleared the stored frames before the replay | copy the stored frames first | F11v3 and FF11: 32/32 stale frames rejected |
| 16 | USB bridge CDC path went silent after an ESP watchdog reboot (enumerated, no data); F9 captured nothing | ESP32-S3 USB stack behavior after a task-watchdog panic (the RA4 kept exchanging heartbeats over the mailbox) | none in firmware; cases rerun; RA4 kernel counters made the authority over the USB record stream | documented in docs/LIMITATIONS.md |
| 17 | The board lost power at 15:43:54; F9 to F11 did not run | external (both VBATT boot counters reset, ESP reset reason power-on; no host tool cuts hub power) | reruns in campaign 2 and the final campaign | results kept as recorded |
| 18 | Soak attempt 2 ended at 841 s (839,900 periods, 0 misses) | host harness exited on an uncaught `SystemExit` while the bridge re-enumerated after a forced crash | harness catches it and retries | tested with a simulated missing port; attempt 3 ran 10,003,440 periods |
| 19 | Soak attempt 1 stopped at 541 s | stopped on request (scheduling), not a failure | restarted later | log kept: `results/phase2/soak_p2_partial_stopped.log` |
| 20 | The **official** bridge resets the RA4 at every ESP boot (observed twice) | upstream configures the RESET and MD pins as outputs before driving them high, which pulses RESET low | the fork drives both HIGH before enabling the outputs | 0 RA4 resets across every fork boot checked (8 final campaign, 7 campaign 1, 2 F11 reruns, 11 soak crashes) |
