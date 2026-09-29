/* Baseline A0: cooperative cyclic executive (superloop) running the Kestrel benchmark workload.
 *
 * Designed as a competent engineer would, not as a strawman:
 *   - a 1 ms minor frame from GPT320 compare-match (same timer hardware as Kestrel/FreeRTOS); the
 *     ISR only advances a frame counter, all work runs in the loop;
 *   - fixed in-frame order by urgency: control every frame, then safety/sensor on even frames,
 *     comms on frame 3 of each 10, a health slice (1 of its 50 work units) on frames 0..49 of each
 *     100 (the 2.3 ms health job cannot run in one piece without breaking the 1 ms frame);
 *   - background (logger) work runs in the slack in 1-unit chunks, checking the frame counter
 *     between chunks, so the worst blocking of the next frame is one chunk.
 * Nothing preempts anything except interrupts. Frame overruns are counted, not hidden. */
#include "apps/common/workload.h"
#include "arch/armv7m/cpu.h"
#include "baselines/common/bench.h"
#include "drivers/adc/adc.h"
#include "drivers/icu/icu.h"
#include "kernel/ipc/mailbox.h"
#include "startup/startup.h"

enum { T_CONTROL = 0, T_SAFETY, T_SENSOR, T_COMMS, T_HEALTH, T_LOGGER };

static ctrl_state_t g_ctrl_state;
static sensor_msg_t g_sensor_state;
static sensor_msg_t mb_sensor_data;
static kmailbox_t mb_sensor = {0, sizeof(sensor_msg_t), (volatile uint8_t *)&mb_sensor_data, 0, 0};
static ctrl_state_t mb_ctrl_data;
static kmailbox_t mb_ctrl = {0, sizeof(ctrl_state_t), (volatile uint8_t *)&mb_ctrl_data, 0, 0};
static volatile uint32_t s_frame; /* advanced by the frame ISR */
static volatile uint32_t s_next_cmp;
static uint32_t s_overruns;
static volatile uint32_t g_violations, g_logger_loops;

static void frame_isr(void)
{
    uint32_t cnt = R_GPT0->GTCNT;
    bench_irq_latency(cnt - s_next_cmp);
    s_next_cmp += BENCH_TICK_TICKS;
    R_GPT0->GTCCR[0] = s_next_cmp;
    s_frame++;
}

static void frame_timer_start(void)
{
    R_MSTP->MSTPCRD &= ~(1u << 5);
    R_GPT0->GTCR = 0u;
    R_GPT0->GTUDDTYC = 1u;
    R_GPT0->GTPR = 0xFFFFFFFFu;
    R_GPT0->GTCNT = 0u;
    s_next_cmp = BENCH_TICK_TICKS;
    R_GPT0->GTCCR[0] = s_next_cmp;
    icu_route(IRQ_LINE_TIMER_CMP, EVT_GPT0_CMP_A, frame_isr, KPRIO_TIMER);
    R_GPT0->GTCR = 1u;
}

static inline uint32_t cycles_now(void)
{
    return kcycles();
}

static void run(uint8_t id, uint32_t seq, uint32_t ideal, uint32_t deadline, void (*job)(void))
{
    uint32_t t0 = bench_now32();
    uint32_t c0 = cycles_now();
    bench_switched_in(id);
    job();
    bench_switched_out(id);
    bench_job(id, seq, ideal, t0, cycles_now() - c0, deadline);
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

void kestrel_boot(void)
{
    bench_init("superloop");
    (void)adc_init();
    __enable_irq();
    frame_timer_start();
    uint32_t done = 0u; /* frames processed */
    uint32_t health_exec = 0u, health_start = 0u, health_seq = 0u;
    for (;;) {
        /* slack: background work in small chunks until the next frame begins */
        while (s_frame == done) {
            bench_switched_in(T_LOGGER);
            (void)work_background(1u);
            bench_switched_out(T_LOGGER);
            g_logger_loops++;
        }
        uint32_t k = done + 1u; /* frame index being served */
        if (s_frame - done > 1u) {
            s_overruns += s_frame - done - 1u; /* frames that began while we were busy */
        }
        done = k;
        uint32_t ideal = k * BENCH_TICK_TICKS;
        run(T_CONTROL, k, ideal, BENCH_TICK_TICKS, job_control);
        if ((k % 2u) == 0u) {
            run(T_SAFETY, k / 2u, ideal, 2u * BENCH_TICK_TICKS, job_safety);
            run(T_SENSOR, k / 2u, ideal, 2u * BENCH_TICK_TICKS, job_sensor);
        }
        if ((k % 10u) == 3u) {
            run(T_COMMS, k / 10u, ideal - 3u * BENCH_TICK_TICKS, 10u * BENCH_TICK_TICKS, job_comms);
        }
        uint32_t hk = k % 100u;
        uint32_t units = g_bench_units[T_HEALTH];
        if (hk < units && hk < 100u) {
            uint32_t c0 = cycles_now();
            if (hk == 0u) {
                health_start = bench_now32();
                health_exec = 0u;
            }
            bench_switched_in(T_HEALTH);
            (void)work_background(1u);
            bench_switched_out(T_HEALTH);
            health_exec += cycles_now() - c0;
            if (hk + 1u == units || hk == 99u) {
                bench_job(T_HEALTH, ++health_seq, (k - hk) * BENCH_TICK_TICKS, health_start,
                          health_exec, 100u * BENCH_TICK_TICKS);
            }
        }
    }
}

void kestrel_main(void);
void kestrel_main(void)
{
}
