# Phase 2 fault model, cases and frozen gates

Frozen 2026-09-30, before the measured Phase 2 campaign (commit that adds this file). Thresholds are
not changed after results are seen; if a threshold proves wrong it is reported as such, not edited.

Scope: **fault isolation / trust-boundary design**. The ESP32-S3 is assumed to fail, stall, reboot,
flood and send garbage. It is not assumed to be a capable attacker (no claim of security against a
malicious adversary with arbitrary firmware on the ESP, see docs/LIMITATIONS.md).

## What the ESP32-S3 can physically do to the RA4M1

| Channel | Worst case | RA4 defense |
|---|---|---|
| UART link (ESP GPIO5 -> RA4 P502 RXD1) | any byte stream at any rate, break (line held low), wrong baud | DMA ring (no per-byte CPU work), per-job byte/frame/time budget in a priority-4 task, receive-error storm guard (mute after 32 errors per window, 20 ms), protocol policy |
| RA4 RESET (ESP GPIO4) | assert reset at any time | **none in software**: a hardware reset line owned by the ESP. The fork never drives it except for the host-requested 1200/2400-baud bootloader entries (upstream behavior); the boot glitch of the stock bridge is fixed. See LIMITATIONS. |
| RA4 MD/BOOT (ESP GPIO9) | select ROM boot on the next reset | only matters together with a reset |
| SWD (CMSIS-DAP on the ESP) | halt, read, write the RA4 | **none**: debug access is total. Kept for development; the trust boundary excludes it. |
| UART0 (ESP GPIO43 -> RA4 SCI9 RX) | garbage into the host command decoder | CRC framing, rejects counted (Phase 1) |

The software trust boundary is the UART link. RESET, MD and SWD are hardware authority the ESP holds
by board design; they are documented, not defended.

## Cases (run on hardware; harness `host/phase2_run.py`)

| ID | Condition | How it is produced |
|---|---|---|
| B0 | stock Arduino bridge 0.5.2, RA4 image with the link service compiled out | `restore_stock_esp_bridge.sh official`; `make APP=phase2 VARIANT=_n1 EXTRA_CFLAGS=-DKESTREL_PHASE2_LINK=0` |
| B1 | fork + link service, mailbox idle (heartbeat/status/metrics, 10 PING/s), no Wi-Fi traffic | default images |
| B2 | normal Wi-Fi: RA4 telemetry forwarded to the host over UDP + 2 Mbit/s UDP host->ESP | `--flood-mbps 2` + `sub` |
| F1 | Wi-Fi throughput load: host floods the ESP sink port at the maximum rate the link takes | `--flood-mbps 0` |
| F2 | ESP CPU saturation: busy task on each core at priority 10 (above the mailbox task) | `fault cpu 15000 10` |
| F3 | ESP heap pressure: heap held down to 8 KB free | `fault mem 15000 8192` |
| F4 | malformed frames, 7 classes round robin at 500/s: garbage, bit flip, oversize length, bad version, unknown type (valid CRC), wrong length (valid CRC), in-session replay (valid CRC) | `fault malformed 15000 500` |
| F5 | mailbox flood: valid in-session PINGs back to back (UART-limited) | `fault flood 15000` |
| F6 | ESP reboot loop, 5 reboots 3 s apart | `fault rebootloop 5 3000` |
| F7 | ESP crash (NULL write -> panic -> reboot) | `fault crash` |
| F7b | ESP hang: mailbox task stops feeding the task watchdog -> TWDT panic -> reboot | `fault hang` |
| F8 | Wi-Fi reconnect storm: SoftAP torn down and restarted 5 times, 1 s apart | `fault apstorm 5 1000` |
| F9 | ESP stops reading the RA4 TX | `fault noread 15000` |
| F10 | ESP stops transmitting | `fault notx 15000` |
| F11 | stale session/replay after ESP reboot: a CONFIG_REQUEST is sent, the ESP reboots, then replays up to 16 frames of the previous boot before its new HELLO and again inside the new session | `config 2 60`, `fault replay` |
| F12 | ESP TX line held low (break) for 15 s, the condition seen while the ESP sat in ROM download mode | `fault txlow 15000` |
| N1 | mailbox disabled entirely (link service compiled out), fork running | `VARIANT=_n1` |
| N2 | mailbox active, ESP silent for 20 s | `fault notx 20000` |
| N3 | F5 flood, RA4 decodes and CRC-validates, then discards (no policy, no replies) | `VARIANT=_n3` + `fault flood 15000` |
| N4 | synthetic communications load: 300 us busy work per link job (the full budget), ESP silent | `VARIANT=_n4` + `fault notx 25000` |

Each case: 30 s capture (baselines and N controls 60 s), fault at t = 5 s, all per-job records of the
six Phase 1 tasks streamed over USB, RA4 kernel counters read before and after, RA4 link evidence
(counters, outage log, trace) read over USB after the ESP has recovered, ESP counters over UDP.

## Metrics
Primary (RA4): critical deadline misses (control, safety, sensor; kernel counters), RA4 resets (VBATT
boot counter), control start latency p50/p99/p99.9/max from per-job records. Secondary: link job
max time, budget hits, drops, malformed/rejected counts, receive errors, storm mutes, outages.
ESP: boot count, reset reason, heap, round-trip time, frames sent per class, UDP sink throughput.

## Gates (frozen)
Reference latency: B1 control start p99 (`p99_B1`). The same 30-60 s capture method as Phase 1.

| Gate | Pass condition |
|---|---|
| G1 idle overhead | B1 vs B0: 0 critical misses in both; control start p99 increase <= 5 us and max increase <= 10 us; link task CPU share <= 3% |
| G2 normal networking | B2: 0 critical misses; control start p99 <= p99_B1 + 5 us |
| G3 ESP saturation | F2 (and F3): 0 critical misses; p99 <= p99_B1 + 5 us |
| G4 network flood | F1 (and F8): 0 critical misses; p99 <= p99_B1 + 5 us |
| G5 malformed traffic | F4 and F12: 0 malformed frames accepted (RA4 per-class counters reconcile with the ESP's per-class send counts; RA4 accepted PINGs never exceed valid PINGs sent), 0 RA4 faults, 0 resets, 0 critical misses |
| G6 mailbox flood | F5: 0 critical misses; link job max <= 400 us (300 us budget + one frame + overheads); excess frames rejected and counted (rate limiter or budget); p99 <= p99_B1 + 5 us |
| G7 ESP reset | F6, F7, F7b: RA4 boot counter unchanged, 0 critical misses |
| G8 recovery | after every ESP reboot a new session is accepted (RA4 `sessions` +1 per ESP boot) and telemetry resumes within 10 s of the ESP's boot; F11: 0 stale frames accepted, the pre-reboot CONFIG_REQUEST is not re-applied |
| G9 long soak | >= 10,000,000 control periods with normal Wi-Fi telemetry and a forced ESP crash every 15 min: 0 unexplained RA4 resets, 0 critical misses, no protocol desynchronization (every outage ends with a new session or resumes; no outage without an ESP-side cause) |

## Kill criteria (any one => Phase 2 NO-GO; Phase 1 stays GO)
- K1: any ESP-load case (B2, F1-F5, F8-F12) raises control start p99 more than 5 us above p99_B1, or
  causes any critical miss.
- K2: any ESP failure blocks RA4 communications processing long enough to cause a critical miss.
- K3: malformed or flooded input corrupts Kestrel state, triggers an RA4 fault, or resets the RA4.
- K4: an ESP reboot resets the RA4 (the fork's own boot, not a host-requested bootloader entry).
- K5: recovery needs manual intervention in any case (other than the host re-joining the SoftAP).
- K6: restoring the stock bridge fails.

## Negative controls, what each separates
- N1 vs B1: cost of the link service itself (task + DMA + UART) with the same ESP firmware.
- N2 vs B1: whether ESP silence (degraded mode) changes RA4 timing.
- N3 vs F5: parser/policy cost vs UART/DMA/decoder cost under the same flood.
- N4 vs B1: whether a worst-case, budget-sized communications job (no ESP involved) is visible in the
  critical tasks' timing; this bounds what any ESP behavior could do through the scheduled path.
