/* Priority-inversion demonstrator (docs/TEST_PLAN.md, section P).
 *
 *   H  prio 7  period 20 ms: sleeps 200 us after release, then locks M_RES (critical section 20 us)
 *   M  prio 5  period 20 ms: sleeps 300 us after release, then burns 5 ms of CPU (never locks)
 *   L  prio 2  period 20 ms: locks M_RES at release, holds it for 1 ms of work, unlocks
 *   comms prio 3: telemetry service (privileged system task), control-free
 * All three share a synchronous release, so every period reproduces the same interleaving:
 * L holds the lock -> H blocks on it -> M preempts L (PLAIN) and H waits for M's whole burst.
 * With the ceiling protocol L runs at ceiling priority while holding the lock, M cannot preempt
 * it, and H waits at most L's remaining critical section. The protocol is switched at run time by
 * app command 0x24 (applied by L while the mutex is free), so both runs use the same binary. */
#include "kernel/kestrel.h"
#include "trace/frame.h"
#include "trace/telemetry.h"

enum { T_H = 0, T_M, T_L, T_COMMS };

typedef struct {
    volatile uint32_t wait_max_ticks;
    volatile uint32_t wait_last_ticks;
    volatile uint32_t samples;
    volatile uint64_t wait_sum_ticks;
    volatile uint8_t want_protocol;
} inv_stats_t;

KESTREL_SHARED static kmutex_t m_res = {KMUTEX_PLAIN, 7u, 0u, 0u, 0u};
KESTREL_SHARED static inv_stats_t g_inv = {0u, 0u, 0u, 0u, KMUTEX_PLAIN};
KESTREL_SHARED static volatile uint32_t g_sink;

KESTREL_STACK(stk_h, 1024);
KESTREL_STACK(stk_m, 1024);
KESTREL_STACK(stk_l, 1024);
KESTREL_STACK(stk_comms, 2048);

static void spin_ticks(uint32_t ticks)
{
    uint32_t t0 = kestrel_time32();
    while ((kestrel_time32() - t0) < ticks) {
        g_sink++;
    }
}

static void h_task(void *a)
{
    (void)a;
    for (;;) {
        uint32_t ts = kestrel_time32();
        kestrel_sleep_us(200u);
        /* Blocking is measured from the instant H wanted the lock (job start + 200 us), not from
         * the lock call: under the ceiling protocol H is kept from running at all while L holds
         * the lock, so timing only the call would hide the blocking. */
        uint32_t t_want = ts + 200u * 48u;
        (void)kestrel_mutex_lock(&m_res);
        uint32_t wait = kestrel_time32() - t_want;
        spin_ticks(20u * 48u);
        (void)kestrel_mutex_unlock(&m_res);
        g_inv.wait_last_ticks = wait;
        if (wait > g_inv.wait_max_ticks) {
            g_inv.wait_max_ticks = wait;
        }
        g_inv.wait_sum_ticks += wait;
        g_inv.samples++;
        kestrel_trace((uint16_t)(wait / 48u)); /* wait in us, visible in the event trace */
        kestrel_wait_period_ts(ts);
    }
}

static void m_task(void *a)
{
    (void)a;
    for (;;) {
        uint32_t ts = kestrel_time32();
        kestrel_sleep_us(300u);
        spin_ticks(5000u * 48u);
        kestrel_wait_period_ts(ts);
    }
}

static void l_task(void *a)
{
    (void)a;
    for (;;) {
        uint32_t ts = kestrel_time32();
        if (m_res.owner == 0u && m_res.protocol != g_inv.want_protocol) {
            m_res.protocol = g_inv.want_protocol; /* safe point: nobody holds the mutex */
        }
        (void)kestrel_mutex_lock(&m_res);
        spin_ticks(1000u * 48u);
        (void)kestrel_mutex_unlock(&m_res);
        kestrel_wait_period_ts(ts);
    }
}

static void comms_task(void *a)
{
    (void)a;
    for (;;) {
        uint32_t ts = kestrel_time32();
        telemetry_service();
        kestrel_wait_period_ts(ts);
    }
}

void kestrel_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    if (cmd == 0x24u && len >= 1u) { /* protocol: 0 plain, 1 ceiling; reset stats */
        g_inv.want_protocol = p[0] != 0u ? KMUTEX_CEILING : KMUTEX_PLAIN;
        g_inv.wait_max_ticks = 0u;
        g_inv.wait_sum_ticks = 0u;
        g_inv.samples = 0u;
    } else if (cmd == 0x21u) {
        uint8_t out[24];
        kput32(&out[0], g_inv.wait_max_ticks);
        kput32(&out[4], g_inv.wait_last_ticks);
        kput32(&out[8], g_inv.samples);
        kput32(&out[12], (uint32_t)g_inv.wait_sum_ticks);
        kput32(&out[16], m_res.protocol);
        kput32(&out[20], m_res.contentions);
        (void)telemetry_send(KF_RESULT, out, sizeof(out));
    }
}

static const kestrel_task_cfg_t k_tasks[] = {
    {"H", h_task, 0, 20000u, 20000u, 7u, 0u, stk_h, sizeof(stk_h), 0u},
    {"M", m_task, 0, 20000u, 20000u, 5u, 0u, stk_m, sizeof(stk_m), 0u},
    {"L", l_task, 0, 20000u, 20000u, 2u, 0u, stk_l, sizeof(stk_l), 0u},
    {"comms", comms_task, 0, 10000u, 10000u, 3u, KTASK_PRIVILEGED, stk_comms, sizeof(stk_comms),
     0u},
};

void kestrel_main(void)
{
    for (uint32_t i = 0; i < sizeof(k_tasks) / sizeof(k_tasks[0]); i++) {
        if (kestrel_task_create(&k_tasks[i]) != KOK) {
            kestrel_panic(0x1A0u + i);
        }
    }
    kestrel_start();
}
