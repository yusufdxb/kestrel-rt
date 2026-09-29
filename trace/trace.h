/* Flight recorder: fixed 8-byte binary events in a static ring, plus 12-byte per-job records.
 *
 * Producers are privileged kernel contexts (ISRs, SVC, PendSV) and are serialized by the kernel
 * critical section, so each ring is single-producer from the ring's point of view; the only
 * consumer is the telemetry service. Full ring => the NEW record is dropped and counted (the
 * oldest data is what the host is about to receive; dropping newest keeps drop accounting exact).
 * Unprivileged tasks emit through the kestrel_trace() system call. No strings anywhere. */
#ifndef KESTREL_TRACE_H
#define KESTREL_TRACE_H
#include <stdint.h>

#include "kernel/ipc/spsc.h"

typedef struct {
    uint32_t timestamp; /* timebase low 32 bits (48 MHz ticks) */
    uint16_t event;     /* (type << 8) | id  (id = task id or IRQ line) */
    uint16_t data;
} trace_event_t;

_Static_assert(sizeof(trace_event_t) == 8u, "trace record must stay 8 bytes");

enum trace_type {
    TE_RELEASE = 1,       /* data: release latency (ticks, saturating) */
    TE_START = 2,         /* data: start latency (ticks, saturating) */
    TE_END = 3,           /* data: exec time /16 ticks (saturating) */
    TE_SWITCH = 4,        /* id: next task; data: previous task */
    TE_ISR_ENTER = 5,     /* id: ICU line */
    TE_ISR_EXIT = 6,
    TE_DEADLINE_MISS = 7, /* data: job sequence low 16 */
    TE_OVERRUN = 8,       /* data: releases skipped */
    TE_QUEUE_OVERFLOW = 9,/* id: channel id; data: reject count low 16 */
    TE_WD_WARNING = 10,   /* data: bitmask of critical tasks without a vote */
    TE_FAULT = 11,        /* data: fault kind */
    TE_RESET_REASON = 12, /* data: reset cause bits */
    TE_BLOCK = 13,        /* data: wait kind */
    TE_WAKE = 14,
    TE_MUTEX = 15,        /* data: 1 lock, 2 unlock, 3 contended */
    TE_USER = 16,         /* id: task; data: user value */
    TE_DEGRADED = 17,     /* data: degraded-flag bits */
    TE_CMD = 18,          /* data: command id applied at a safe point */
};

#define JR_FLAG_MISS      0x01u
#define JR_FLAG_OVERRUN   0x02u
#define JR_FLAG_SATURATED 0x04u

typedef struct {
    uint8_t task;
    uint8_t flags;
    uint16_t seq;            /* job number (low 16 bits) */
    uint16_t release_lat;    /* ticks from ideal release to the release ISR, saturating */
    uint16_t start_lat;      /* ticks from ideal release to first dispatch, saturating */
    uint16_t exec_16;        /* execution time in units of 16 ticks (333 ns), saturating */
    uint16_t response_16;    /* completion - ideal release in units of 16 ticks, saturating */
} job_record_t;

_Static_assert(sizeof(job_record_t) == 12u, "job record must stay 12 bytes");

typedef struct {
    uint32_t events;
    uint32_t events_dropped;
    uint32_t jobs;
    uint32_t jobs_dropped;
} trace_stats_t;

void trace_init(void);
/* Kernel contexts only (privileged, IRQs may be enabled; takes the critical section itself). */
void trace_emit(uint8_t type, uint8_t id, uint16_t data);
void trace_emit_slow(uint8_t type, uint8_t id, uint16_t data);
extern uint32_t g_trace_mask;
/* Caller holds kcrit. Disabled types cost one load + test (no call). */
static inline void trace_emit_locked(uint8_t type, uint8_t id, uint16_t data)
{
    if ((g_trace_mask & (1u << type)) != 0u) {
        trace_emit_slow(type, id, data);
    }
}
void trace_job_locked(const job_record_t *jr);
/* Which event types are recorded (bit per type). Job records have their own per-task mask. */
void trace_set_mask(uint32_t type_mask, uint8_t job_task_mask);
uint8_t trace_job_mask(void);

/* Consumer side (telemetry service only). */
kq_spsc_t *trace_event_ring(void);
kq_spsc_t *trace_job_ring(void);
const trace_stats_t *trace_stats(void);

static inline uint16_t sat16(uint32_t v)
{
    return (v > 0xFFFFu) ? (uint16_t)0xFFFFu : (uint16_t)v;
}
#endif
