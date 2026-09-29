/* Context-switch microbenchmark on FreeRTOS V11.1.0: identical method to apps/ctxbench/main.c.
 * Signalling uses direct-to-task notifications, FreeRTOS's fastest primitive (favours FreeRTOS).
 *   B (prio 4): t0; xTaskNotifyGive(A); ... resumes: t3
 *   A (prio 6): ulTaskNotifyTake() returns: t1 (signal_to_wake = t1 - t0); t2; block again
 *              (block_to_resume = t3 - t2). */
#include "FreeRTOS.h"
#include "apps/common/lathist.h"
#include "arch/armv7m/cpu.h"
#include "baselines/common/bench.h"
#include "drivers/icu/icu.h"
#include "startup/startup.h"
#include "task.h"
#include "trace/frame.h"

static volatile uint32_t s_t0, s_t2;
static lathist_t s_wake, s_resume;
static TaskHandle_t s_a;
static volatile uint32_t s_next_cmp;
extern void xPortSysTickHandler(void);
void vPortSetupTimerInterrupt(void);

static void gpt_tick_isr(void)
{
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
    s_next_cmp = BENCH_TICK_TICKS;
    R_GPT0->GTCCR[0] = s_next_cmp;
    icu_route(IRQ_LINE_TIMER_CMP, EVT_GPT0_CMP_A, gpt_tick_isr,
              configLIBRARY_LOWEST_INTERRUPT_PRIORITY);
    R_GPT0->GTCR = 1u;
}

static void a_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_t2 = bench_now32();
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t t1 = bench_now32();
        lathist_add(&s_wake, t1 - s_t0);
    }
}

static void b_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t w = bench_now32();
        while ((bench_now32() - w) < 48u * 50u) {
        }
        s_t0 = bench_now32();
        xTaskNotifyGive(s_a);
        uint32_t t3 = bench_now32();
        lathist_add(&s_resume, t3 - s_t2);
    }
}

static void comms_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, 10u);
        bench_service();
    }
}

static void send_hist(uint8_t which, const lathist_t *h)
{
    for (uint8_t part = 0; part < 2u; part++) {
        uint8_t out[20 + 64 * 4];
        out[0] = which;
        out[1] = part;
        kput16(&out[2], 1u << LATHIST_SHIFT);
        kput32(&out[4], h->count);
        kput32(&out[8], h->max);
        kput32(&out[12], h->min);
        kput32(&out[16], h->overflow);
        for (uint32_t b = 0; b < 64u; b++) {
            kput32(&out[20u + 4u * b], h->bins[part * 64u + b]);
        }
        bench_send(KF_RESULT, out, sizeof(out));
    }
}

void bench_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    (void)p;
    (void)len;
    if (cmd == 0x21u) {
        send_hist(0u, &s_wake);
        send_hist(1u, &s_resume);
    }
}

static StackType_t stk_a[128], stk_b[128], stk_c[512];
static StaticTask_t tcb_a, tcb_b, tcb_c;

void kestrel_boot(void)
{
    bench_init("freertos-ctx");
    s_a = xTaskCreateStatic(a_task, "A", 128, 0, 6, stk_a, &tcb_a);
    TaskHandle_t b = xTaskCreateStatic(b_task, "B", 128, 0, 4, stk_b, &tcb_b);
    TaskHandle_t c = xTaskCreateStatic(comms_task, "comms", 512, 0, 3, stk_c, &tcb_c);
    vTaskSetApplicationTaskTag(s_a, (TaskHookFunction_t)1);
    vTaskSetApplicationTaskTag(b, (TaskHookFunction_t)2);
    vTaskSetApplicationTaskTag(c, (TaskHookFunction_t)4);
    vTaskStartScheduler();
    for (;;) {
    }
}

void kestrel_main(void);
void kestrel_main(void)
{
}
