/* Baseline A2: Kestrel benchmark workload on Zephyr v4.2.0.
 * Threads with the same priority order, periods and workload kernels; periodic release with
 * absolute timeouts (k_sleep(K_TIMEOUT_ABS_TICKS)). Telemetry, ADC and timebase use the same
 * register-level drivers and measurement shim as the superloop and FreeRTOS baselines; their ICU
 * lines are attached with IRQ_CONNECT to the shared dispatcher.
 * Ideal release instants: the kernel tick grid, mapped onto the GPT320 timebase by sampling the
 * GPT counter at a tick boundary at start-up (docs/BENCHMARKS.md explains the residual offset). */
#include "apps/common/workload.h"
#include "baselines/common/bench.h"
#include "drivers/adc/adc.h"
#include "drivers/icu/icu.h"
#include "kernel/ipc/mailbox.h"

#include <zephyr/irq.h>
#include <zephyr/kernel.h>

void kestrel_irq_dispatch(void);

#define REG32(a)  (*(volatile uint32_t *)(a))
#define GPT0_BASE 0x40078000u
#define MSTPCRD   0x40047008u

enum { T_CONTROL = 0, T_SAFETY, T_SENSOR, T_COMMS, T_HEALTH, T_LOGGER };

static ctrl_state_t g_ctrl_state;
static sensor_msg_t g_sensor_state;
static sensor_msg_t mb_sensor_data;
static kmailbox_t mb_sensor = {0, sizeof(sensor_msg_t), (volatile uint8_t *)&mb_sensor_data, 0, 0};
static ctrl_state_t mb_ctrl_data;
static kmailbox_t mb_ctrl = {0, sizeof(ctrl_state_t), (volatile uint8_t *)&mb_ctrl_data, 0, 0};
static volatile uint32_t g_violations, g_logger_loops;

static uint32_t s_gpt_at_tick0; /* GPT320 count at kernel tick s_tick0 */
static int64_t s_tick0;
static uint32_t s_gpt_per_tick;

static void isr_trampoline(const void *arg)
{
    (void)arg;
    kestrel_irq_dispatch(); /* reads IPSR: same dispatch path as Kestrel */
}

static void timebase_start(void)
{
    REG32(MSTPCRD) &= ~(1u << 5);
    REG32(GPT0_BASE + 0x2C) = 0u;          /* GTCR: stop, saw, PCLKD/1 */
    REG32(GPT0_BASE + 0x30) = 1u;          /* GTUDDTYC: up */
    REG32(GPT0_BASE + 0x64) = 0xFFFFFFFFu; /* GTPR */
    REG32(GPT0_BASE + 0x48) = 0u;          /* GTCNT */
    REG32(GPT0_BASE + 0x2C) = 1u;          /* start */
    s_gpt_per_tick = 48000000u / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
    /* align to a tick boundary: spin until the tick count changes, then sample the GPT */
    int64_t t = k_uptime_ticks();
    while (k_uptime_ticks() == t) {
    }
    s_gpt_at_tick0 = bench_now32();
    s_tick0 = k_uptime_ticks();
}

static inline uint32_t ideal_of(int64_t tick)
{
    return s_gpt_at_tick0 + (uint32_t)(tick - s_tick0) * s_gpt_per_tick;
}

static void periodic(uint8_t id, uint32_t period_ms, void (*job)(void))
{
    int64_t period = k_ms_to_ticks_ceil64(period_ms);
    int64_t next = k_uptime_ticks() + period;
    uint32_t seq = 0u;
    for (;;) {
        k_sleep(K_TIMEOUT_ABS_TICKS(next));
        uint32_t t_start = bench_now32();
        uint32_t c0 = bench_task_cycles(id);
        job();
        bench_job(id, ++seq, ideal_of(next), t_start, bench_task_cycles(id) - c0,
                  period_ms * 48000u);
        next += period;
    }
}

static void job_control(void)
{
    sensor_msg_t m;
    uint32_t seq;
    if (kmb_read(&mb_sensor, &m, &seq) == KMB_OK) {
        work_control(&g_ctrl_state, &m, g_bench_units[T_CONTROL]);
        kmb_write(&mb_ctrl, &g_ctrl_state);
    }
}

static void job_safety(void)
{
    ctrl_state_t c;
    sensor_msg_t m;
    uint32_t cs, ms;
    if (kmb_read(&mb_ctrl, &c, &cs) == KMB_OK && kmb_read(&mb_sensor, &m, &ms) == KMB_OK) {
        g_violations += work_safety(&c, &m, g_bench_units[T_SAFETY]);
    }
}

static void job_sensor(void)
{
    static uint32_t synthetic;
    adc_sample_t s;
    uint32_t sample = adc_take_latest(&s) ? s.code : ((synthetic += 37u) & 0x3FFFu);
    work_sensor(&g_sensor_state, sample, g_bench_units[T_SENSOR]);
    kmb_write(&mb_sensor, &g_sensor_state);
}

static void job_comms(void)
{
    bench_service();
}

static void job_health(void)
{
    (void)work_background(g_bench_units[T_HEALTH]);
}

static void th_control(void *a, void *b, void *c)
{
    periodic(T_CONTROL, 1u, job_control);
}
static void th_safety(void *a, void *b, void *c)
{
    periodic(T_SAFETY, 2u, job_safety);
}
static void th_sensor(void *a, void *b, void *c)
{
    periodic(T_SENSOR, 2u, job_sensor);
}
static void th_comms(void *a, void *b, void *c)
{
    periodic(T_COMMS, 10u, job_comms);
}
static void th_health(void *a, void *b, void *c)
{
    periodic(T_HEALTH, 100u, job_health);
}
static void th_logger(void *a, void *b, void *c)
{
    for (;;) {
        (void)work_background(g_bench_units[T_LOGGER]);
        g_logger_loops++;
    }
}

/* Zephyr: lower number = higher priority. Same order as Kestrel/FreeRTOS; stacks match (1 KB,
 * comms 2 KB). Threads start after main() has set up the timebase and drivers (delay 10 ms). */
K_THREAD_DEFINE(tid_control, 1024, th_control, NULL, NULL, NULL, 1, K_FP_REGS, 10);
K_THREAD_DEFINE(tid_safety, 1024, th_safety, NULL, NULL, NULL, 2, K_FP_REGS, 10);
K_THREAD_DEFINE(tid_sensor, 1024, th_sensor, NULL, NULL, NULL, 3, K_FP_REGS, 10);
K_THREAD_DEFINE(tid_comms, 2048, th_comms, NULL, NULL, NULL, 5, 0, 10);
K_THREAD_DEFINE(tid_health, 1024, th_health, NULL, NULL, NULL, 6, 0, 10);
K_THREAD_DEFINE(tid_logger, 1024, th_logger, NULL, NULL, NULL, 7, 0, 10);

/* Per-thread CPU accounting through Zephyr's user tracing hooks (CONFIG_TRACING_USER), the same
 * switch-time accounting Kestrel and the FreeRTOS baseline use. */
#include <zephyr/tracing/tracing.h>
static uint32_t task_index(const struct k_thread *t)
{
    if (t == tid_control) {
        return T_CONTROL;
    }
    if (t == tid_safety) {
        return T_SAFETY;
    }
    if (t == tid_sensor) {
        return T_SENSOR;
    }
    if (t == tid_comms) {
        return T_COMMS;
    }
    if (t == tid_health) {
        return T_HEALTH;
    }
    if (t == tid_logger) {
        return T_LOGGER;
    }
    return BENCH_MAX_TASKS; /* idle, main: ignored by the shim */
}

void sys_trace_thread_switched_in_user(void)
{
    bench_switched_in(task_index(k_current_get()));
}

void sys_trace_thread_switched_out_user(void)
{
    bench_switched_out(task_index(k_current_get()));
}

int main(void)
{
    IRQ_CONNECT(IRQ_LINE_UART_RXI, 2, isr_trampoline, NULL, 0);
    IRQ_CONNECT(IRQ_LINE_UART_TEI, 2, isr_trampoline, NULL, 0);
    IRQ_CONNECT(IRQ_LINE_UART_ERI, 2, isr_trampoline, NULL, 0);
    IRQ_CONNECT(IRQ_LINE_DMA_TX, 2, isr_trampoline, NULL, 0);
    IRQ_CONNECT(IRQ_LINE_ADC, 2, isr_trampoline, NULL, 0);
    IRQ_CONNECT(IRQ_LINE_ADC_TRIG, 2, isr_trampoline, NULL, 0);
    timebase_start();
    bench_init("zephyr-4.2.0");
    (void)adc_init();
    return 0;
}
