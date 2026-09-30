# Phase 2 mailbox protocol ("KX")

Portable C in `link/xlink.{h,c}`, compiled unchanged into the RA4 image, the ESP fork (staged by
`esp/build.sh`) and the host tests (`tests/test_xlink.c`).

## Frame
| Offset | Size | Field |
|---|---:|---|
| 0 | 2 | magic 0x4B 0x58 ("KX") |
| 2 | 1 | version (1) |
| 3 | 1 | type |
| 4 | 2 | session id of the sender's session (RA4 -> ESP: low 16 bits of the RA4 boot epoch) |
| 6 | 4 | sequence, per direction and session, first frame 1, strictly increasing |
| 10 | 2 | payload length N, <= 256 |
| 12 | N | payload, little endian |
| 12+N | 2 | CRC-16/CCITT-FALSE over bytes 2 .. 12+N-1 |
Overhead 14 bytes. The decoder resynchronises on the magic after any error; every error class is counted.

## Message types
| Dir | Type | Payload | RA4 handling |
|---|---|---|---|
| ESP->RA4 | 0x01 HELLO | u32 RA4 epoch seen, u32 ESP boot count | starts a new session (rules below); RA4 answers with its FAULT record |
| | 0x02 PING | u32 nonce | PONG (nonce, RA4 ms) |
| | 0x03 CONFIG_REQUEST | u8 param, pad[3], u32 value | whitelist only (below); CONFIG_ACK |
| | 0x04 TELEMETRY_CONTROL | u32 class mask (bit0 STATUS, bit1 METRICS, bit2 TRACE) | link-task state only |
| | 0x05 TIME_SYNC | u64 ESP time us | metadata only, never touches the RA4 timebase |
| RA4->ESP | 0x81 STATUS | jobs/misses/max start latency of the 3 critical tasks, boot count | every 500 ms |
| | 0x82 TRACE | up to 16 link trace events (8-byte `trace_event_t`) | when events exist |
| | 0x83 FAULT | boot count, reset cause, crash-record-present, crash record | on each new session |
| | 0x84 METRICS | control jobs/misses in window, frames ok/malformed/rate-rejected, RX overrun bytes, link job max, loss events, critical misses, control max start latency, rejected | every 1 s (100-5000 ms configurable) |
| | 0x85 HEARTBEAT | epoch, RA4 ms, current session, last ESP seq, control jobs/misses, degraded flag | every 100 ms |
| | 0x86 PONG, 0x87 CONFIG_ACK | | replies |

## RA4 acceptance policy (`xl_rx_accept`), in order
1. type must be an ESP->RA4 type, 2. payload length must equal that type's exact length,
3. token bucket 400 frames/s, burst 40 (rejected frames past this point are counted `rej_rate`),
4. HELLO: must echo this RA4 boot's epoch, session id non-zero, not the current and not one of the last
   32 retired ids, seq = 1; the previous session is retired,
5. other types: a session must exist, session id must match, seq > last (replay), seq - last <= 64 (jump).
Each rule has its own counter. No message names an address, function, register, priority or period.

Configuration whitelist (`xl_config_valid`): 1 logger work units 1..400, 2 health work units 1..200,
3 METRICS period 100..5000 ms. Anything else is rejected and acknowledged as rejected. Accepted values
are applied by the health task at the start of its next job (its safe point) or, for the METRICS
period, by the link task itself. Critical tasks are unreachable.

## Sessions and recovery
- The RA4 epoch is `(VBATT boot count << 16) ^ cycle counter` and changes on every RA4 boot.
- The ESP learns the epoch from HEARTBEAT, sends HELLO with a random non-zero session id and seq 1,
  and treats the session as established when a HEARTBEAT carries its id. No confirmation within
  300 ms: new id, new HELLO. An epoch change (RA4 reboot) starts a new session.
- After an ESP reboot every frame of the previous boot is stale: before the new HELLO it fails the
  sequence check of the still-current old session (replay); after the new HELLO it fails the session
  check; an old HELLO fails the retired-id check.

## RA4 service budget (link task, priority 4, period 5 ms, privileged, noncritical)
Per job at most 512 bytes read from the ring, 16 frames decoded to a verdict, 300 us of work (checked
between 64-byte chunks); bytes past the frame budget are kept for the next job. TX: double buffer,
512 bytes per job (1 Mbaud carries 625 bytes in 5 ms); frames that do not fit are dropped and counted.
Loss detection: no accepted frame for 250 ms -> degraded, outage logged (start, end) in a 16-entry log.
The link task votes to the supervisor as a monitored noncritical task; the watchdog never depends on it.

## Host side
`host/esp_ctrl.py` (UDP 47000: `stats`, `sub`, `config P V`, `fault ...`; UDP 47002: sink),
`host/phase2_stats.py` (RA4 link evidence over USB, app commands 0x40 LINK_STATS, 0x41 LINK_OUTAGES).
