/* Telemetry service: the only producer of the UART TX byte ring. Runs inside the privileged comms
 * task. Everything it does is nonblocking: if the link is slower than the data, records stay in
 * (and then overflow) the kernel rings, which count the drops. Control never waits on it. */
#ifndef KESTREL_TELEMETRY_H
#define KESTREL_TELEMETRY_H
#include <stdint.h>

#include "trace/frame.h"

typedef struct {
    uint32_t frames_sent;
    uint32_t bytes_queued;
    uint32_t ring_full;      /* frame build skipped: TX ring had no room */
    uint32_t cmds_ok;
    uint32_t cmds_bad;
    uint32_t stall_until_lo; /* TX stall injection active until (ticks, low 32) */
} telemetry_stats_t;

void telemetry_init(void);
/* One service pass: parse received commands, frame pending records, start DMA. Bounded work. */
void telemetry_service(void);
/* Queue an arbitrary frame (used by apps for KF_RESULT payloads). Returns 0 if no room. */
uint32_t telemetry_send(uint8_t type, const uint8_t *payload, uint16_t len);
const telemetry_stats_t *telemetry_stats(void);

/* Application command hook (weak default ignores). Called from the comms task; the application
 * must apply changes only at its own safe points (e.g. publish to a config mailbox). */
void kestrel_app_command(uint8_t cmd, const uint8_t *payload, uint16_t len);
#endif
