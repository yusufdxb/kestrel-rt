/* Context-switch microbenchmark (same method as baselines/freertos/ctx_main.c):
 *   B (prio 4) : t0 = now; signal A; (A preempts) ... resumes: t3 = now
 *   A (prio 6) : blocked on flags; wakes: t1 = now  -> signal_to_wake = t1 - t0
 *                t2 = now; block on flags again     -> block_to_resume = t3 - t2
 * Both paths go through the full system-call + scheduler + PendSV + MPU retarget + exception
 * return. Timestamps: GPT320 (48 MHz), read by the tasks themselves. Build flag CTX_PRIV=1 runs A
 * and B privileged and switches the MPU off (like-for-like with a non-MPU kernel). */
#include "apps/common/lathist.h"
#include "kernel/kestrel.h"
#include "trace/frame.h"
#include "trace/telemetry.h"

#ifndef CTX_PRIV
#define CTX_PRIV 0
#endif
#define AB_FLAGS (CTX_PRIV ? KTASK_PRIVILEGED : 0u)

typedef struct {
    volatile uint32_t t0, t2;
    lathist_t wake;
    lathist_t resume;
} ctx_shared_t;

KESTREL_SHARED static ctx_shared_t g_ctx;
KESTREL_SHARED static kflags_t f_a;

KESTREL_STACK(stk_a, 512);
KESTREL_STACK(stk_b, 512);
KESTREL_STACK(stk_comms, 2048);

static void a_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_ctx.t2 = kestrel_time32();
        (void)kestrel_flags_wait(&f_a, 1u, KFLAGS_ANY | KFLAGS_CLEAR, KWAIT_FOREVER);
        uint32_t t1 = kestrel_time32();
        lathist_add(&g_ctx.wake, t1 - g_ctx.t0);
    }
}

static void b_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t w = kestrel_time32();
        while ((kestrel_time32() - w) < 48u * 50u) { /* 50 us between rounds */
        }
        g_ctx.t0 = kestrel_time32();
        kestrel_flags_set(&f_a, 1u);
        uint32_t t3 = kestrel_time32();
        lathist_add(&g_ctx.resume, t3 - g_ctx.t2);
    }
}

static void comms_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t ts = kestrel_time32();
        telemetry_service();
        kestrel_wait_period_ts(ts);
    }
}

static void send_hist(uint8_t which, const lathist_t *h)
{
    for (uint8_t part = 0; part < 2u; part++) {
        uint8_t out[8 + 16 + 64 * 4];
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
        (void)telemetry_send(KF_RESULT, out, 20u + 256u);
    }
}

void kestrel_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    (void)p;
    (void)len;
    if (cmd == 0x21u) {
        send_hist(0u, &g_ctx.wake);
        send_hist(1u, &g_ctx.resume);
    }
}

static const kestrel_task_cfg_t k_tasks[] = {
    {"A", a_task, 0, 0u, 0u, 6u, AB_FLAGS, stk_a, sizeof(stk_a), 0u},
    {"B", b_task, 0, 0u, 0u, 4u, AB_FLAGS, stk_b, sizeof(stk_b), 0u},
    {"comms", comms_task, 0, 10000u, 10000u, 3u, KTASK_PRIVILEGED, stk_comms, sizeof(stk_comms), 0u},
};

void kestrel_main(void)
{
    for (uint32_t i = 0; i < sizeof(k_tasks) / sizeof(k_tasks[0]); i++) {
        if (kestrel_task_create(&k_tasks[i]) != KOK) {
            kestrel_panic(0xC70u + i);
        }
    }
    kestrel_start();
}
