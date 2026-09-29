/* Mutexes.
 *
 * KMUTEX_PLAIN: blocking mutex with priority-ordered handoff and NO inversion control. It exists
 * to demonstrate unbounded priority inversion in the fault campaign; production channels use
 * KMUTEX_CEILING.
 *
 * KMUTEX_CEILING: immediate priority ceiling protocol (IPCP, "highest locker"). On lock the owner
 * is raised to level 2*ceiling+1, above every task that may lock the mutex. On a single core this
 * means no user of the mutex can run while it is held, so lock never blocks, blocking of a
 * higher-priority task is bounded by one critical section, chained blocking and deadlock between
 * ceiling mutexes are impossible, and the kernel needs no wait queue. The price is that the
 * ceiling must be known statically (true for Kestrel's static task set) and that the owner
 * delays medium-priority tasks for the critical section even when nobody contends. */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"
#include "trace/trace.h"

int32_t k_mutex_lock(ktcb_t *t, uint32_t *frame, kmutex_t *m);
int32_t k_mutex_unlock(ktcb_t *t, kmutex_t *m);

static ktcb_t *owner_of(const kmutex_t *m)
{
    return (m->owner == 0u) ? 0 : &g_k.tcb[m->owner - 1u];
}

int32_t k_mutex_lock(ktcb_t *t, uint32_t *frame, kmutex_t *m)
{
    if (m->protocol == KMUTEX_CEILING) {
        uint8_t prio = (uint8_t)(t->base_level / 2u);
        uint8_t lvl = (uint8_t)(2u * m->ceiling + 1u);
        if (prio > m->ceiling || m->owner != 0u || t->ceil_depth >= 4u ||
            (g_k.level_owner[lvl] != 0 && g_k.level_owner[lvl] != t)) {
            m->contentions++;
            return KE_PERM; /* ceiling misconfigured: a user outranks the ceiling */
        }
        m->owner = (uint8_t)(t->id + 1u);
        t->ceil_saved[t->ceil_depth++] = t->level;
        if (lvl > t->level) {
            k_set_level(t, lvl);
        }
        trace_emit_locked(TE_MUTEX, t->id, 1u);
        return KOK;
    }
    if (m->owner == 0u) {
        m->owner = (uint8_t)(t->id + 1u);
        trace_emit_locked(TE_MUTEX, t->id, 1u);
        return KOK;
    }
    if (owner_of(m) == t) {
        return KE_STATE; /* not recursive */
    }
    m->contentions++;
    trace_emit_locked(TE_MUTEX, t->id, 3u);
    t->wait_obj = m;
    t->wait_frame = frame;
    k_block(t, KW_MUTEX);
    k_reschedule();
    return KOK; /* delivered again by k_wake at handoff */
}

int32_t k_mutex_unlock(ktcb_t *t, kmutex_t *m)
{
    if (owner_of(m) != t) {
        return KE_PERM;
    }
    trace_emit_locked(TE_MUTEX, t->id, 2u);
    if (m->protocol == KMUTEX_CEILING) {
        m->owner = 0u;
        uint8_t back = t->ceil_saved[--t->ceil_depth];
        if (back != t->level) {
            k_set_level(t, back);
        }
        k_reschedule();
        return KOK;
    }
    /* PLAIN: hand over to the highest-priority waiter, if any. */
    for (int32_t lvl = (int32_t)KNUM_LEVELS - 1; lvl >= 0; lvl--) {
        ktcb_t *w = g_k.level_owner[lvl];
        if (w != 0 && w->state == KS_BLOCKED && w->wait == KW_MUTEX && w->wait_obj == m) {
            m->owner = (uint8_t)(w->id + 1u);
            k_wake(w, (uint32_t)KOK);
            k_reschedule();
            return KOK;
        }
    }
    m->owner = 0u;
    return KOK;
}
