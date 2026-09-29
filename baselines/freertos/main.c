/* Baseline A1: the Kestrel benchmark workload on FreeRTOS V11.1.0 (ARM_CM4F port).
 * Same tasks, priorities, periods, workload kernels (apps/common/workload.c), same lock-free
 * mailboxes and ADC path, same UART DMA telemetry shim. Only the kernel differs. */
#include "FreeRTOS.h"
#include "apps/common/workload.h"
#include "arch/armv7m/cpu.h"
#include "baselines/common/bench.h"
#include "drivers/adc/adc.h"
#include "drivers/icu/icu.h"
#include "kernel/ipc/mailbox.h"
#include "startup/startup.h"
#include "task.h"

enum { T_CONTROL = 0, T_SAFETY, T_SENSOR, T_COMMS, T_HEALTH, T_LOGGER, T_COUNT };

static ctrl_state_t g_ctrl_state;
static sensor_msg_t g_sensor_state;
static sensor_msg_t mb_sensor_data;
static kmailbox_t mb_sensor = {0, sizeof(sensor_msg_t), (volatile uint8_t *)&mb_sensor_data, 0, 0};
static ctrl_state_t mb_ctrl_data;
static kmailbox_t mb_ctrl = {0, sizeof(ctrl_state_t), (volatile uint8_t *)&mb_ctrl_data, 0, 0};
static volatile uint32_t g_violations, g_logger_loops;

/* ---- tick from GPT320 compare A: tick k at GPT time s_tick_origin + k * 48000 exactly ---- */
static volatile uint32_t s_tick_origin;
static volatile uint32_t s_next_cmp;
void vPortSetupTimerInterrupt(void);
extern void xPortSysTickHandler(void);

static void gpt_tick_isr(void)
{
    uint32_t cnt = R_GPT0->GTCNT;
    bench_irq_latency(cnt - s_next_cmp);
    s_next_cmp += BENCH_TICK_TICKS;
    R_GPT0->GTCCR[0] = s_next_cmp;
    xPortSysTickHandler();
}

void vPortSetupTimerInterrupt(void)
{
    R_MSTP->MSTPCRD &= ~(1u << 5);
    R_GPT0->GTCR = 0u;
    R_GPT0->GTUDDTYC = 1u;
    R_GPT0->GTPR = 0xFFFFFFFFu;
    R_GPT0->GTCNT = 0u;
    s_tick_origin = 0u;
    s_next_cmp = BENCH_TICK_TICKS;
    R_GPT0->GTCCR[0] = s_next_cmp;
    /* FreeRTOS rule: the tick runs at the kernel (lowest) interrupt priority. */
    icu_route(IRQ_LINE_TIMER_CMP, EVT_GPT0_CMP_A, gpt_tick_isr,
              configLIBRARY_LOWEST_INTERRUPT_PRIORITY);
    R_GPT0->GTCR = 1u;
}

static inline uint32_t ideal_of(TickType_t tick)
{
    return s_tick_origin + (uint32_t)tick * BENCH_TICK_TICKS;
}

typedef struct {
    uint8_t id;
    TickType_t period;
} targ_t;

static void periodic_loop(uint8_t id, TickType_t period, void (*job)(void))
{
    TickType_t last = xTaskGetTickCount();
    uint32_t seq = 0u;
    for (;;) {
        vTaskDelayUntil(&last, period);
        uint32_t t_start = bench_now32();
        uint32_t c0 = bench_task_cycles(id);
        job();
        bench_job(id, ++seq, ideal_of(last), t_start, bench_task_cycles(id) - c0,
                  (uint32_t)period * BENCH_TICK_TICKS);
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

static void t_control(void *a)
{
    (void)a;
    periodic_loop(T_CONTROL, 1u, job_control);
}
static void t_safety(void *a)
{
    (void)a;
    periodic_loop(T_SAFETY, 2u, job_safety);
}
static void t_sensor(void *a)
{
    (void)a;
    periodic_loop(T_SENSOR, 2u, job_sensor);
}
static void t_comms(void *a)
{
    (void)a;
    periodic_loop(T_COMMS, 10u, job_comms);
}
static void t_health(void *a)
{
    (void)a;
    periodic_loop(T_HEALTH, 100u, job_health);
}
static void t_logger(void *a)
{
    (void)a;
    for (;;) {
        (void)work_background(g_bench_units[T_LOGGER]);
        g_logger_loops++;
    }
}

/* Same stack sizes as the Kestrel benchmark: 1 KB each, comms 2 KB. */
static StackType_t stk_control[256], stk_safety[256], stk_sensor[256], stk_comms[512],
    stk_health[256], stk_logger[256];
static StaticTask_t tcb[T_COUNT];

void kestrel_boot(void)
{
    bench_init("freertos-11.1.0");
    (void)adc_init();
    static const struct {
        TaskFunction_t fn;
        const char *name;
        UBaseType_t prio;
        uint32_t words;
        StackType_t *stack;
    } def[T_COUNT] = {
        {t_control, "control", 7, 256, stk_control}, {t_safety, "safety", 6, 256, stk_safety},
        {t_sensor, "sensor", 5, 256, stk_sensor},    {t_comms, "comms", 3, 512, stk_comms},
        {t_health, "health", 2, 256, stk_health},    {t_logger, "logger", 1, 256, stk_logger},
    };
    for (uint32_t i = 0; i < T_COUNT; i++) {
        TaskHandle_t h = xTaskCreateStatic(def[i].fn, def[i].name, def[i].words, 0, def[i].prio,
                                           def[i].stack, &tcb[i]);
        vTaskSetApplicationTaskTag(h, (TaskHookFunction_t)(uintptr_t)(i + 1u));
    }
    vTaskStartScheduler();
    for (;;) {
    }
}

/* Kestrel's startup expects this symbol; FreeRTOS owns everything after kestrel_boot. */
void kestrel_main(void);
void kestrel_main(void)
{
}
