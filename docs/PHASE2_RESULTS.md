# Phase 2 results: RA4M1 real-time domain vs ESP32-S3 communications domain

All numbers measured on the physical UNO R4 WiFi on 2026-09-30. Phase 1 results (docs/BENCHMARKS.md,
docs/TEST_PLAN.md) are separate and unchanged. Gates were frozen before the campaign in
docs/PHASE2_FAULT_MODEL.md. Per-case JSON: `results/phase2/*.json` (raw job-record streams:
`results/raw/p2_*.bin`, hashes in `results/MANIFEST.sha256`).

Images of the final campaign and soak:
- RA4M1: `make APP=phase2` at the final commit, sha256 of kestrel.bin `14c04cb3...8cb`
- ESP32-S3: fork `esp/build.sh`, KestrelBridge.bin sha256 `f1a96a1c...bdd4`, written to app0 only

## 1. Baselines and negative controls (control task, start latency in us, 60 s each)

| Case | What runs | p50 | p99 | p99.9 | max | link task CPU | critical misses |
|---|---|---:|---:|---:|---:|---:|---:|
| B0 | stock bridge 0.5.2, link compiled out | 24.29 | 44.54 | 44.54 | 48.25 | - | 0 |
| N1 | fork, link compiled out | 24.29 | 44.54 | 44.54 | 46.29 | - | 0 |
| B1 | fork + link, mailbox idle (10 PING/s + RA4 heartbeat/status/metrics) | 24.96 | 47.88 | 47.88 | 50.96 | 0.83% | 0 |
| B1 repeat | same | 24.96 | 47.88 | 47.88 | 51.46 | 0.83% | 0 |
| B2 | + RA4 telemetry forwarded over Wi-Fi + 1.8 Mbit/s UDP delivered to the ESP | 24.96 | 47.88 | 47.88 | 49.96 | 0.83% | 0 |
| N2 | ESP silent 20 s of 60 (RA4 degraded) | 24.96 | 47.88 | 47.92 | 51.54 | 0.74% | 0 |
| N3 | UART-saturating flood, RA4 validates then discards | 24.96 | 47.88 | 47.88 | 49.96 | 0.71% | 0 |
| N4 | 300 us synthetic work per link job (the full budget), ESP silent | 24.96 | 47.88 | 47.92 | 51.88 | 6.73% | 0 |

B0 reproduces Phase 1 (24.6 / 44.9 / 49.0 us). **The only measurable Phase 2 cost is a fixed +3.3 us at
p99** (44.54 -> 47.88) that appears as soon as the link task exists and does not change with what it does:
idle (B1), degraded (N2), parse-and-discard flood (N3), or a budget-sized busy job (N4) all give 47.88.
Most likely explanation (ASSUMPTION, not isolated): the extra 5 ms release handled in the kernel timer ISR
(the highest-priority interrupt) occasionally coincides with the control release. **G1 PASS** (+3.3 us p99
<= 5, +2.7 to +3.2 us max <= 10, link CPU 0.83% <= 3%).

## 2. Fault campaign, final images (30 s each, fault at t = 5 s; FF prefix = final campaign)

| Case | Fault | RA4 resets | critical misses | p99 / max (us) | control records lost on USB | link outcome |
|---|---|---:|---:|---|---:|---|
| F1 | UDP flood host->ESP, 62,080 x 1,400 B offered in 20 s; 5.9 Mbit/s delivered | 0 | 0 | 47.88 / 50.83 | 10 | no outage |
| F2 | busy task on both ESP cores, priority 10, 15 s | 0 | 0 | 47.88 / 49.62 | 11,700 (ESP USB loop starved) | no outage |
| F3 | ESP heap held to 5.7 KB free, 15 s | 0 | 0 | 47.88 / 49.96 | 0 | no outage |
| F4 | 15,000 malformed frames, 7 classes (1,000/s) | 0 | 0 | 47.88 / 48.92 | 0 | 0 accepted (below) |
| F5 | 83,711 valid frames in 15 s (5,580/s, UART-bound) | 0 | 0 | 47.88 / 49.96 | 30 | 6,335 accepted, 8,526 rate-limited, 1.21 MB dropped by budget, all counted |
| F8 | SoftAP torn down/restarted 5 x | 0 | 0 | 47.88 / 51.54 | 20 | no outage |
| F9 | ESP stops reading the RA4 for 15 s | 0 | 0 | 47.88 / 49.75 | 0 | RA4 TX never blocks |
| F10 | ESP stops transmitting for 15 s | 0 | 0 | 47.88 / 49.75 | 0 | 1 outage 15.09 s, resumed, same session |
| F12 | ESP TX line held low (break) for 15 s | 0 | 0 | 47.88 / 50.83 | 20 | 19,200 receive errors, 600 receiver mutes, 1 outage 15.29 s, resumed |
| F11 | CONFIG_REQUEST, ESP reboot, 16 stale frames before and 16 inside the new session | 0 | 0 | 47.88 / 50.00 | 2,240 | 32/32 stale rejected (16 replay, 16 session); config applied once, not re-applied |
| F6 | 5 ESP reboots 3 s apart | 0 | 0 | 47.88 / 50.83 | 11,240 | 5 outages (1.68 s total), 5 new sessions |
| F7 | ESP panic (NULL write) | 0 | 0 | 47.88 / 49.96 | 2,320 | 1 outage 0.35 s, new session |
| F7b | ESP mailbox task hangs -> ESP task watchdog panic (reset reason 6) | 0 | 0 | 47.88 / 49.75 | 4,300 | 1 outage 2.34 s, new session |

- Across the 8 ESP reboots of the final campaign the RA4 VBATT boot counter stayed at 5 and its uptime was
  continuous. Records "lost on USB" are job records that could not cross the ESP's USB bridge while it was
  rebooting or starved; the RA4 kernel counters (read afterwards) cover every job, and the kernel's own
  control start-latency maximum stayed 44.98 to 47.73 us in every case (B1: 47.02 to 47.52).
- Link task worst job over the whole final campaign: 314 us wall (RX work budget 200 us, time checked every
  16 bytes), under the 400 us gate.
- ESP side: reset reasons as intended (3 software for reboot/replay, 4 panic, 6 task watchdog); heap
  minimum 5,688 B during F3 without an ESP reset; no unplanned ESP reset in any case.

F4 per-class accounting (ESP sent / RA4 counter): bit flip 2,143 / CRC 1,961; oversize length 2,143 /
length 2,138; bad version 2,143 / version 2,067; unknown type 2,143 / type 2,090; wrong length 2,143 /
length 2,091; in-session replay 2,142 / replay 2,116; random garbage 2,143 x 20 B / 69,134 bytes skipped.
Per-class totals fall slightly short because a corrupted length field makes the decoder swallow the next
frame, which is then rejected by CRC instead of by its own class. **Accepted: 442 frames, against 453 valid
frames sent** (a replayed PING being accepted would push accepted above valid).

## 3. Gates

| Gate | Result |
|---|---|
| G1 idle overhead | **PASS** (section 1) |
| G2 normal networking | **PASS**: B2 0 misses, p99 47.88 |
| G3 ESP saturation | **PASS**: F2, F3 0 misses, p99 47.88 |
| G4 network flood | **PASS**: F1, F8 0 misses, p99 47.88 |
| G5 malformed traffic | **PASS**: F4 0 accepted, F12 storm bounded, 0 faults, 0 resets, 0 misses |
| G6 mailbox flood | **PASS on the final image**; **FAILED in campaign 1** (see section 4) |
| G7 ESP reset | **PASS**: F6, F7, F7b 0 RA4 resets, 0 misses |
| G8 recovery | **PASS**: new session after every ESP boot, outages 0.3 to 2.3 s; F11 32/32 stale rejected |
| G9 long soak | see section 5 |

Kill criteria K1-K6: none triggered in the final campaign. K4 detail: the **stock** bridge resets the RA4
on its own boot (observed twice: in the recovery gate and when B0 was prepared); the fork never did in any
boot where the RA4 boot counter was checked: 8 in the final campaign, 7 in campaign 1 (F6, F7, F7b), 2 in
the F11 reruns, and the fork flashes before N1 and F11v3 (and the first two fork flashes).

## 4. What failed on the way (campaign 1 and 2, kept, not rewritten)
1. **ESP ROM mode produced about 1.17 million receive-error interrupts on the RA4** (ESP TX line low
   while the ESP sat in download mode). No deadline was missed, but it proved the ESP could drive RA4
   interrupt load. Fix: receive-error storm guard (32 errors per window -> 20 ms mute). Reproduced as F12:
   19,200 errors in 15 s instead of about 1.5 million.
2. **RA4 TX discarded PONG/ACK/FAULT frames** (buffer reset before the periodic emitter). Fix: double buffer.
3. **G6 failed in campaign 1**: under F5 the link job reached 493 us wall / 507 us execution (frozen limit
   400 us); the time budget was checked only between 64-byte chunks and not before TX emission. Critical
   tasks were unaffected (0 misses, p99 47.88). Fix: check every 16 bytes, 200 us RX budget. Campaign 2
   and the final campaign: 312-314 us.
4. **Permanent desynchronization after F12 (campaign 1)**: the ESP consumed sequence numbers while not
   transmitting; after 15 s its frames jumped by more than 64 and the RA4 rejected them all, while the
   ESP, still receiving RA4 heartbeats, never started a new session. Fix: no sequence number without a
   transmission, and the ESP starts a new session when the RA4's acknowledged sequence (in HEARTBEAT) stops
   advancing for 500 ms. Final campaign: resumed, 0 resyncs needed.
5. **USB bridge CDC path went silent** after the F7b ESP reboot in campaign 1 (device enumerated, no
   data), so F9 captured nothing; the board then lost power at 15:43:54 (both VBATT boot counters reset,
   ESP reset reason power-on; cause external, no host tool cuts hub power), and F10/F11 did not start.
   The RA4 kept exchanging heartbeats with the ESP over the mailbox during the silent period.
   These cases were rerun (campaign 2 and final).
6. F11 in campaign 2 exercised only the pre-HELLO replay (a test bug cleared the stored frames); fixed and
   rerun (F11v3, final FF11).

## 5. G9 long soak
Pending (running: >= 10,000,000 control periods, Wi-Fi telemetry, forced ESP crash every 15 min).

## 6. Maximum rates (measured)
- Mailbox link: 1,000,000 baud; F5 offered 5,580 valid frames/s (the UART limit for 18-byte frames).
  RA4 accepts at most 400 frames/s sustained (burst 40) by policy; it decoded about 1,080 frames/s under
  the flood within its budget and dropped the rest at the ring, counted.
- Wi-Fi into the ESP (UDP sink, SoftAP; host distance not measured): 5.9 Mbit/s delivered under a maximum-rate flood.
- RA4 communications budget: 300 us per 5 ms job (6%); measured link CPU 0.83% idle, 2.97% under F5.
