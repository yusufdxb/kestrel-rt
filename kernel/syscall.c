/* System-call layer. Task-side wrappers issue `svc #n` with arguments in r0-r3; SVC_Handler
 * (context_switch.S) passes the caller's stacked exception frame to k_svc_dispatch, which writes
 * results back into the stacked r0/r1. Blocking calls record the frame so the result can be
 * written when the task is woken (the hardware frame stays in place while the task is blocked). */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"
#include "kernel/khist.h"
#include "kernel/time/timebase.h"
#include "safety/crashlog/crashlog.h"
#include "safety/fault/fault.h"
#include "trace/trace.h"

enum {
    SVC_WAIT_PERIOD = 1,
    SVC_SLEEP = 2,
    SVC_YIELD = 3,
    SVC_NOW = 4,
    SVC_VOTE = 5,
    SVC_TRACE = 6,
    SVC_ASSERT = 7,
    SVC_FLAGS_WAIT = 8,
    SVC_FLAGS_SET = 9,
    SVC_MUTEX_LOCK = 10,
    SVC_MUTEX_UNLOCK = 11,
    SVC_TASK_EXIT = 14,
};

/* ---------- task side ---------- */
#define SVC0(n) __asm volatile("svc %0" ::"I"(n) : "memory")

#define SVC_CALL(num, a0, a1, a2, a3, out)                                                         \
    do {                                                                                           \
        register uint32_t r0_ __asm("r0") = (a0);                                                  \
        register uint32_t r1_ __asm("r1") = (a1);                                                  \
        register uint32_t r2_ __asm("r2") = (a2);                                                  \
        register uint32_t r3_ __asm("r3") = (a3);                                                  \
        __asm volatile("svc %4" : "+r"(r0_), "+r"(r1_) : "r"(r2_), "r"(r3_), "I"(num) : "memory"); \
        (out)[0] = r0_;                                                                            \
        (out)[1] = r1_;                                                                            \
        (void)(out)[0];                                                                            \
    } while (0)

void kestrel_wait_period(void)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_WAIT_PERIOD, 0u, 0u, 0u, 0u, o);
}

void kestrel_wait_period_ts(uint32_t job_start_ts)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_WAIT_PERIOD, job_start_ts, 1u, 0u, 0u, o);
}

void kestrel_sleep_us(uint32_t us)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_SLEEP, us, 0u, 0u, 0u, o);
}

void kestrel_yield(void)
{
    SVC0(SVC_YIELD);
}

uint64_t kestrel_now(void)
{
    uint32_t o[2];
    SVC_CALL(SVC_NOW, 0u, 0u, 0u, 0u, o);
    return ((uint64_t)o[1] << 32) | o[0];
}

void kestrel_health_vote(void)
{
    SVC0(SVC_VOTE);
}

void kestrel_trace(uint16_t value)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_TRACE, value, 0u, 0u, 0u, o);
}

void kestrel_assert_fail(uint32_t line)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_ASSERT, line, 0u, 0u, 0u, o);
    for (;;) {
    }
}

uint32_t kestrel_flags_wait(kflags_t *f, uint32_t mask, uint32_t mode, uint32_t timeout_us)
{
    uint32_t o[2];
    SVC_CALL(SVC_FLAGS_WAIT, (uint32_t)(uintptr_t)f, mask, mode, timeout_us, o);
    return o[0];
}

void kestrel_flags_set(kflags_t *f, uint32_t bits)
{
    uint32_t o[2];
    (void)o;
    SVC_CALL(SVC_FLAGS_SET, (uint32_t)(uintptr_t)f, bits, 0u, 0u, o);
}

kstatus_t kestrel_mutex_lock(kmutex_t *m)
{
    uint32_t o[2];
    SVC_CALL(SVC_MUTEX_LOCK, (uint32_t)(uintptr_t)m, 0u, 0u, 0u, o);
    return (kstatus_t)(int32_t)o[0];
}

kstatus_t kestrel_mutex_unlock(kmutex_t *m)
{
    uint32_t o[2];
    SVC_CALL(SVC_MUTEX_UNLOCK, (uint32_t)(uintptr_t)m, 0u, 0u, 0u, o);
    return (kstatus_t)(int32_t)o[0];
}

/* ---------- kernel side ---------- */
extern uint32_t __app_shared_start, __app_shared_end;

bool k_user_ptr_ok(const ktcb_t *caller, const void *p, uint32_t size)
{
    uintptr_t a = (uintptr_t)p;
    if (caller->privileged) {
        return a >= 0x20000000u && a + size <= 0x20008000u;
    }
    return a >= (uintptr_t)&__app_shared_start && a + size <= (uintptr_t)&__app_shared_end;
}

/* flags.c / mutex.c */
uint32_t k_flags_wait(ktcb_t *t, uint32_t *frame, kflags_t *f, uint32_t mask, uint32_t mode,
                      uint32_t timeout_us, uint64_t now);
void k_flags_set_locked(kflags_t *f, uint32_t bits);
int32_t k_mutex_lock(ktcb_t *t, uint32_t *frame, kmutex_t *m);
int32_t k_mutex_unlock(ktcb_t *t, kmutex_t *m);

void k_svc_dispatch(uint32_t *frame, uint32_t svc_num, uint32_t exc_return)
{
    (void)exc_return;
    kcrit_t s = kcrit_enter();
    uint32_t c0 = kcycles();
    g_k.svc_count++;
    ktcb_t *t = g_k.current;
    /* The 64-bit time read costs ~70 cycles: only the calls that need it pay for it. */
    uint64_t now = (svc_num == SVC_WAIT_PERIOD || svc_num == SVC_SLEEP || svc_num == SVC_NOW ||
                    svc_num == SVC_VOTE ||
                    (svc_num == SVC_FLAGS_WAIT && frame[3] != 0u && frame[3] != KWAIT_FOREVER))
                       ? kt_now()
                       : 0u;
    switch (svc_num) {
    case SVC_WAIT_PERIOD:
        if (frame[1] == 1u && t->job_active) {
            t->start_lat = frame[0] - (uint32_t)t->release_ideal; /* task-observed */
            t->start_observed = true;
        }
        k_job_end_locked(t, now);
        k_events_changed(t);
        k_reschedule();
        break;
    case SVC_SLEEP:
        k_block(t, KW_SLEEP);
        t->wake_at = now + kt_us_to_ticks(frame[0]);
        k_events_changed(t);
        k_reschedule();
        break;
    case SVC_YIELD:
        k_reschedule();
        break;
    case SVC_NOW:
        frame[0] = (uint32_t)now;
        frame[1] = (uint32_t)(now >> 32);
        break;
    case SVC_VOTE:
        t->last_vote = now;
        break;
    case SVC_TRACE:
        trace_emit_locked(TE_USER, t->id, (uint16_t)frame[0]);
        break;
    case SVC_ASSERT:
        kfault_record_software(t, frame, KFAULT_ASSERT, frame[0]);
        k_task_fault(t, KFAULT_ASSERT);
        k_reschedule();
        break;
    case SVC_TASK_EXIT:
        kfault_record_software(t, frame, KFAULT_TASK_EXIT, 0u);
        k_task_fault(t, KFAULT_TASK_EXIT);
        k_reschedule();
        break;
    case SVC_FLAGS_WAIT: {
        kflags_t *f = (kflags_t *)(uintptr_t)frame[0];
        if (!k_user_ptr_ok(t, f, sizeof(*f))) {
            frame[0] = 0u;
            break;
        }
        frame[0] = k_flags_wait(t, frame, f, frame[1], frame[2], frame[3], now);
        break;
    }
    case SVC_FLAGS_SET: {
        kflags_t *f = (kflags_t *)(uintptr_t)frame[0];
        if (k_user_ptr_ok(t, f, sizeof(*f))) {
            k_flags_set_locked(f, frame[1]);
        }
        break;
    }
    case SVC_MUTEX_LOCK: {
        kmutex_t *m = (kmutex_t *)(uintptr_t)frame[0];
        frame[0] = k_user_ptr_ok(t, m, sizeof(*m)) ? (uint32_t)k_mutex_lock(t, frame, m)
                                                   : (uint32_t)KE_PERM;
        break;
    }
    case SVC_MUTEX_UNLOCK: {
        kmutex_t *m = (kmutex_t *)(uintptr_t)frame[0];
        frame[0] =
            k_user_ptr_ok(t, m, sizeof(*m)) ? (uint32_t)k_mutex_unlock(t, m) : (uint32_t)KE_PERM;
        break;
    }
    default:
        frame[0] = (uint32_t)KE_INVAL;
        break;
    }
    uint32_t dt = kcycles() - c0;
    khist_add(&g_hist_svc, dt);
    if (dt > g_k.max_crit_cycles) {
        g_k.max_crit_cycles = dt;
    }
    kcrit_exit(s);
}
