/* Measurement + telemetry shim shared by the baselines (superloop, FreeRTOS, Zephyr).
 *
 * It reuses Kestrel's register-level drivers (SCI9 + DMAC0 UART, ICU dispatcher) and the same
 * frame format / 12-byte job records, so host/experiment.py analyses every system identically.
 * No kernel services are used: records are pushed under a short PRIMASK section, the TX ring is
 * drained by bench_service() from whatever low-priority context the baseline provides. */
#ifndef KESTREL_BENCH_H
#define KESTREL_BENCH_H
#include "trace/trace.h"

#include <stdint.h>

#define BENCH_MAX_TASKS  8u
#define BENCH_TICK_TICKS 48000u /* 1 ms at 48 MHz */

typedef struct {
    uint32_t jobs[BENCH_MAX_TASKS];
    uint32_t misses[BENCH_MAX_TASKS];
    uint32_t overruns[BENCH_MAX_TASKS];
    uint32_t wcet_cyc[BENCH_MAX_TASKS];
    uint32_t max_start[BENCH_MAX_TASKS];
    uint64_t run_cyc[BENCH_MAX_TASKS];
} bench_stats_t;

extern volatile uint16_t g_bench_units[BENCH_MAX_TASKS];

void bench_init(const char *system_name);
/* Called by a task (or superloop slot) at job end. `ideal` = nominal release on the GPT0
 * timebase; `t_start` = task-observed start; `exec_cycles` = CPU time of this job. */
void bench_job(uint8_t task, uint32_t seq, uint32_t ideal, uint32_t t_start, uint32_t exec_cycles,
               uint32_t deadline_ticks);
/* Low-priority context only: RX commands, status, drain records, kick DMA. Bounded work. */
void bench_service(void);
/* Accounting hooks (context-switch hooks or superloop slot boundaries). */
void bench_switched_in(uint32_t task);
void bench_switched_out(uint32_t task);
uint32_t bench_task_cycles(uint32_t task); /* call from the running task itself */
/* Application hooks: extra host commands, raw frame send (low-priority context only). */
void bench_app_command(uint8_t cmd, const uint8_t *p, uint16_t len);
void bench_send(uint8_t type, const uint8_t *p, uint16_t len);
/* Hardware ISR latency sample (compare target -> handler), same histogram as Kestrel's. */
void bench_irq_latency(uint32_t ticks);
static inline uint32_t bench_now32(void)
{
    return *(volatile uint32_t *)0x40078048u; /* GPT320.GTCNT, shared timebase */
}
#endif
