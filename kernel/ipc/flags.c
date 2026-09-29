/* Event flags: 32-bit groups, wait-any / wait-all with optional clear-on-match and timeout.
 * Wakeup order is strictly by priority (level scan high -> low), which also decides who consumes
 * bits under KFLAGS_CLEAR. Work per set is bounded by the task count (8 + idle). */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"
#include "kernel/time/timebase.h"
#include "trace/trace.h"

uint32_t k_flags_wait(ktcb_t *t, uint32_t *frame, kflags_t *f, uint32_t mask, uint32_t mode,
                      uint32_t timeout_us, uint64_t now);
void k_flags_set_locked(kflags_t *f, uint32_t bits);

static uint32_t match(uint32_t have, uint32_t mask, uint32_t mode)
{
    uint32_t m = have & mask;
    if ((mode & KFLAGS_ALL) != 0u) {
        return (m == mask) ? m : 0u;
    }
    return m;
}

uint32_t k_flags_wait(ktcb_t *t, uint32_t *frame, kflags_t *f, uint32_t mask, uint32_t mode,
                      uint32_t timeout_us, uint64_t now)
{
    if (mask == 0u) {
        return 0u;
    }
    uint32_t m = match(f->bits, mask, mode);
    if (m != 0u) {
        if ((mode & KFLAGS_CLEAR) != 0u) {
            f->bits &= ~m;
        }
        return m;
    }
    if (timeout_us == 0u) {
        return 0u;
    }
    t->wait_obj = f;
    t->wait_mask = mask;
    t->wait_mode = (uint8_t)mode;
    t->wait_frame = frame;
    k_block(t, KW_FLAGS);
    t->wake_at = (timeout_us == KWAIT_FOREVER) ? KNO_TIME : now + kt_us_to_ticks(timeout_us);
    k_recompute_alarm(now);
    k_reschedule();
    return 0u; /* overwritten at wakeup via the saved frame pointer */
}

/* Called with the kernel critical section held (from SVC or from the ISR wrapper). */
void k_flags_set_locked(kflags_t *f, uint32_t bits)
{
    f->bits |= bits;
    for (int32_t lvl = (int32_t)KNUM_LEVELS - 1; lvl >= 0; lvl--) {
        ktcb_t *w = g_k.level_owner[lvl];
        if (w == 0 || w->state != KS_BLOCKED || w->wait != KW_FLAGS || w->wait_obj != f) {
            continue;
        }
        uint32_t m = match(f->bits, w->wait_mask, w->wait_mode);
        if (m == 0u) {
            continue;
        }
        if ((w->wait_mode & KFLAGS_CLEAR) != 0u) {
            f->bits &= ~m;
        }
        k_wake(w, m);
        trace_emit_locked(TE_WAKE, w->id, (uint16_t)m);
    }
    k_reschedule();
}

void kestrel_flags_set_isr(kflags_t *f, uint32_t bits)
{
    kcrit_t s = kcrit_enter();
    k_flags_set_locked(f, bits);
    kcrit_exit(s);
}
