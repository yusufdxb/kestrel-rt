#include "trace/trace.h"

#include "arch/armv7m/cpu.h"
#include "kernel/time/timebase.h"

#define TRACE_EVENTS 128u
#define TRACE_JOBS   128u

static trace_event_t s_ev_storage[TRACE_EVENTS];
static job_record_t s_job_storage[TRACE_JOBS];
static kq_spsc_t s_ev = {0, 0, TRACE_EVENTS - 1u, sizeof(trace_event_t), (uint8_t *)s_ev_storage, 0, 0};
static kq_spsc_t s_jobs = {0, 0, TRACE_JOBS - 1u, sizeof(job_record_t), (uint8_t *)s_job_storage, 0, 0};
static trace_stats_t s_stats;
uint32_t g_trace_mask;
static uint8_t s_job_mask;

void trace_init(void)
{
    /* Default: rare/diagnostic events only. High-rate events (per job, per switch) are enabled by
     * the host for timeline captures; per-job timing always flows through job records. */
    g_trace_mask = 0xFFFFFFFFu & ~((1u << TE_SWITCH) | (1u << TE_ISR_ENTER) | (1u << TE_ISR_EXIT) |
                                  (1u << TE_RELEASE) | (1u << TE_START) | (1u << TE_END) |
                                  (1u << TE_BLOCK) | (1u << TE_WAKE) | (1u << TE_MUTEX));
    s_job_mask = 0xFFu;
}

void trace_set_mask(uint32_t type_mask, uint8_t job_task_mask)
{
    g_trace_mask = type_mask;
    s_job_mask = job_task_mask;
}

uint8_t trace_job_mask(void)
{
    return s_job_mask;
}

void trace_emit_slow(uint8_t type, uint8_t id, uint16_t data)
{
    trace_event_t e = {kt_now32(), (uint16_t)(((uint16_t)type << 8) | id), data};
    if (kq_spsc_push(&s_ev, &e)) {
        s_stats.events++;
    } else {
        s_stats.events_dropped++;
    }
}

void trace_emit(uint8_t type, uint8_t id, uint16_t data)
{
    kcrit_t s = kcrit_enter();
    trace_emit_locked(type, id, data);
    kcrit_exit(s);
}

void trace_job_locked(const job_record_t *jr)
{
    if ((s_job_mask & (1u << jr->task)) == 0u) {
        return;
    }
    if (kq_spsc_push(&s_jobs, jr)) {
        s_stats.jobs++;
    } else {
        s_stats.jobs_dropped++;
    }
}

kq_spsc_t *trace_event_ring(void)
{
    return &s_ev;
}

kq_spsc_t *trace_job_ring(void)
{
    return &s_jobs;
}

const trace_stats_t *trace_stats(void)
{
    return &s_stats;
}
