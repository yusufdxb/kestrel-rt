/* Fixed-priority preemptive scheduler with tickless release timing.
 *
 * Ready set: one bit per priority level in g_k.ready_levels; the highest set bit (CLZ) selects the
 * task in O(1). Level = 2 * priority; odd levels are used only while a task holds a
 * priority-ceiling mutex (level 2*ceiling + 1), so the holder outranks every user of that mutex
 * without sharing a level with any of them. Level 0 is the idle task, which is always ready.
 *
 * All kernel state changes happen inside one short PRIMASK critical section per kernel entry
 * (timer ISR, SVC, PendSV selection); the longest one is measured (g_k.max_*_cycles).
 */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"
#include "kernel/khist.h"
#include "kernel/time/timebase.h"
#include "safety/fault/fault.h"
#include "trace/trace.h"

kernel_t g_k;
volatile uint32_t g_kprof[8]; /* diagnostic stage stamps of the last timer ISR */
#ifdef KESTREL_PROFILE
#define KPROF(i) (g_kprof[(i)] = kcycles())
#else
#define KPROF(i) ((void)0)
#endif
static uint32_t s_prev_pendsv_entry;

static inline uint32_t highest_level(void)
{
    return 31u - __CLZ(g_k.ready_levels);
}

static void record_start(ktcb_t *t, uint32_t now32)
{
    t->job_started = true;
    uint32_t lat = now32 - (uint32_t)t->release_ideal; /* modulo-2^32 delta, valid < 89 s */
    t->start_lat = lat;
    if (lat > t->max_start_lat) {
        t->max_start_lat = lat;
    }
    if (lat < t->min_start_lat) {
        t->min_start_lat = lat;
    }
    trace_emit_locked(TE_START, t->id, sat16(lat));
}

void k_make_ready(ktcb_t *t)
{
    t->state = KS_READY;
    t->wait = KW_NONE;
    t->wake_at = KNO_TIME;
    g_k.ready_levels |= 1u << t->level;
}

void k_wake(ktcb_t *t, uint32_t result)
{
    t->wait_result = result;
    if (t->wait_frame != 0) {
        t->wait_frame[0] = result;
        t->wait_frame = 0;
    }
    t->wait_obj = 0;
    k_make_ready(t);
}

void k_block(ktcb_t *t, kwait_t why)
{
    t->state = KS_BLOCKED;
    t->wait = (uint8_t)why;
    g_k.ready_levels &= ~(1u << t->level);
    trace_emit_locked(TE_BLOCK, t->id, (uint16_t)why);
}

void k_set_level(ktcb_t *t, uint8_t level)
{
    bool ready = (g_k.ready_levels & (1u << t->level)) != 0u && t->state == KS_READY;
    if (ready) {
        g_k.ready_levels &= ~(1u << t->level);
    }
    if (t->level != t->base_level) {
        g_k.level_owner[t->level] = 0; /* leaving a ceiling level */
    }
    t->level = level;
    g_k.level_owner[level] = t;
    if (ready) {
        g_k.ready_levels |= 1u << level;
    }
}

void k_reschedule(void)
{
    if (!g_k.started) {
        return;
    }
    if (g_k.level_owner[highest_level()] != g_k.current) {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}

void k_release_locked(ktcb_t *t, uint64_t now)
{
    uint64_t ideal = t->next_release;
    if (t->job_active) {
        /* Overrun: the previous job is still running. Skip releases (no backlog), keep the grid. */
        uint64_t skipped = (now - ideal) / t->period + 1u;
        t->next_release = ideal + skipped * t->period;
        t->overruns += (uint32_t)skipped;
        trace_emit_locked(TE_OVERRUN, t->id, sat16((uint32_t)skipped));
        return;
    }
    t->releases++;
    t->job_active = true;
    t->job_started = false;
    t->deadline_flagged = false;
    t->release_ideal = ideal;
    t->abs_deadline = ideal + t->deadline;
    uint32_t lat = (uint32_t)(now - ideal);
    t->release_lat = lat;
    if (lat > t->max_release_lat) {
        t->max_release_lat = lat;
    }
    t->next_release = ideal + t->period;
    if (t->next_release <= now) {
        /* Released very late (e.g. after a long non-preemptible stall): realign to the grid. */
        uint64_t skipped = (now - t->next_release) / t->period + 1u;
        t->next_release += skipped * t->period;
        t->overruns += (uint32_t)skipped;
        trace_emit_locked(TE_OVERRUN, t->id, sat16((uint32_t)skipped));
    }
    t->job_exec = 0u;
    trace_emit_locked(TE_RELEASE, t->id, sat16(lat));
    if (t->state == KS_BLOCKED && t->wait == KW_PERIOD) {
        k_make_ready(t);
    }
}

void k_job_end_locked(ktcb_t *t, uint64_t now)
{
    uint32_t cyc = kcycles();
    uint32_t exec = t->job_exec + (cyc - t->switched_in);
    t->run_cycles += cyc - t->switched_in;
    t->switched_in = cyc;
    t->job_exec = 0u;
    if (!t->job_active) {
        return; /* aperiodic task: wait_period is a no-op yield */
    }
    uint32_t resp = (uint32_t)(now - t->release_ideal);
    uint8_t flags = 0u;
    if (now > t->abs_deadline) {
        flags |= JR_FLAG_MISS;
        if (!t->deadline_flagged) {
            t->deadline_flagged = true;
            t->deadline_misses++;
            trace_emit_locked(TE_DEADLINE_MISS, t->id, (uint16_t)t->jobs);
        }
    }
    t->jobs++;
    if (exec > t->wcet) {
        t->wcet = exec;
    }
    if (resp > t->max_response) {
        t->max_response = resp;
    }
    if (t->overruns != 0u && (t->next_release - t->release_ideal) > t->period) {
        flags |= JR_FLAG_OVERRUN;
    }
    uint32_t e16 = exec >> 4;
    uint32_t r16 = resp >> 4;
    if (e16 > 0xFFFFu || r16 > 0xFFFFu || t->release_lat > 0xFFFFu || t->start_lat > 0xFFFFu) {
        flags |= JR_FLAG_SATURATED;
    }
    if (t->start_observed) {
        flags |= JR_FLAG_TASK_OBSERVED;
        t->start_observed = false;
    }
    job_record_t jr = {
        t->id,      flags,     (uint16_t)t->jobs, sat16(t->release_lat), sat16(t->start_lat),
        sat16(e16), sat16(r16)};
    trace_job_locked(&jr);
    trace_emit_locked(TE_END, t->id, sat16(e16));
    t->job_active = false;

    if (now >= t->next_release) {
        k_release_locked(t, now); /* next job already due: continue without blocking */
        if (t->job_active) {
            record_start(t, (uint32_t)now);
        }
    } else {
        k_block(t, KW_PERIOD);
    }
}

static uint64_t task_next_event(const ktcb_t *t)
{
    uint64_t ne = KNO_TIME;
    if (t->state == KS_UNUSED || t->state == KS_FAULTED) {
        return ne;
    }
    if (t->period != 0u) {
        ne = t->next_release;
    }
    if (t->job_active && !t->deadline_flagged && t->abs_deadline < ne) {
        ne = t->abs_deadline;
    }
    if (t->state == KS_BLOCKED && t->wake_at < ne) {
        ne = t->wake_at;
    }
    return ne;
}

static void program_alarm(uint64_t when)
{
    g_k.alarm_at = when;
    kt_alarm_set(when);
}

void k_events_changed(ktcb_t *t)
{
    t->next_event = task_next_event(t);
    if (g_k.started && t->next_event < g_k.alarm_at) {
        program_alarm(t->next_event);
    }
}

void k_recompute_alarm(uint64_t now)
{
    (void)now;
    uint64_t next = g_k.next_supervisor;
    for (uint32_t i = 0; i < KNUM_TCB; i++) {
        ktcb_t *t = &g_k.tcb[i];
        t->next_event = task_next_event(t);
        if (t->next_event < next) {
            next = t->next_event;
        }
    }
    program_alarm(next);
}

void k_alarm_isr(void)
{
    uint32_t c0 = kcycles();
    kcrit_t s = kcrit_enter();
    KPROF(0);
    uint64_t now = kt_now();
    KPROF(1);
    g_k.alarm_count++;
    if (g_k.started) {
        /* One pass: process due tasks and compute the next alarm. Tasks whose cached next_event is
         * in the future cost one 64-bit compare. */
        uint64_t next = g_k.next_supervisor;
        for (uint32_t i = 0; i < KNUM_TCB; i++) {
            ktcb_t *t = &g_k.tcb[i];
            if (now >= t->next_event) {
                if (t->state != KS_UNUSED && t->state != KS_FAULTED) {
                    if (t->period != 0u && now >= t->next_release) {
                        k_release_locked(t, now);
                    }
                    if (t->job_active && !t->deadline_flagged && now >= t->abs_deadline) {
                        t->deadline_flagged = true;
                        t->deadline_misses++;
                        trace_emit_locked(TE_DEADLINE_MISS, t->id, (uint16_t)t->jobs);
                    }
                    if (t->state == KS_BLOCKED && now >= t->wake_at) {
                        k_wake(t, 0u); /* timeout (sleep ends the same way) */
                    }
                }
                t->next_event = task_next_event(t);
            }
            if (t->next_event < next) {
                next = t->next_event;
            }
        }
        KPROF(2);
        if (now >= g_k.next_supervisor) {
            k_dwt_guard();
            k_supervisor_tick(now);
            uint64_t p = kt_us_to_ticks(KSUPERVISOR_PERIOD_US);
            g_k.next_supervisor += p;
            if (g_k.next_supervisor <= now) {
                g_k.next_supervisor = now + p;
            }
            if (g_k.next_supervisor < next) {
                next = g_k.next_supervisor;
            }
        }
        KPROF(3);
        program_alarm(next);
        KPROF(4);
        k_reschedule();
        KPROF(5);
    }
    kcrit_exit(s);
    uint32_t dt = kcycles() - c0;
    KPROF(6);
    khist_add(&g_hist_alarm, dt);
    if (dt > g_k.max_alarm_cycles) {
        g_k.max_alarm_cycles = dt;
    }
}

/* PendSV C part: account the outgoing task, select the next, retarget the MPU. */
uint32_t *kestrel_sched_switch(uint32_t *old_sp)
{
    kcrit_t s = kcrit_enter();
    uint32_t c0 = kcycles();
    ktcb_t *cur = g_k.current;
    cur->sp = old_sp;
    uint32_t ran = c0 - cur->switched_in;
    cur->job_exec += ran;
    cur->run_cycles += ran;
    if (cur->cfg != 0 && cur->cfg->stack_base[KSTACK_CANARY_WORD] != KSTACK_CANARY &&
        cur->state != KS_FAULTED) {
        cur->canary_hits++;
        const uint32_t *hw = old_sp + 10u + (((old_sp[0] & 0x10u) == 0u) ? 16u : 0u);
        kfault_record_software(cur, hw, KFAULT_STACK_CANARY, 0u);
        k_task_fault(cur, KFAULT_STACK_CANARY);
    }
    /* Duration of the previous PendSV (entry and exit stamps written by context_switch.S). */
    if (g_k.pendsv_count != 0u) {
        khist_add(&g_hist_pendsv, k_pendsv_exit - s_prev_pendsv_entry);
    }
    s_prev_pendsv_entry = k_pendsv_entry;
    ktcb_t *next = g_k.level_owner[highest_level()];
    g_k.pendsv_count++;
    if (next != cur) {
        g_k.context_switches++;
        trace_emit_locked(TE_SWITCH, next->id, cur->id);
    }
    next->switched_in = c0;
    if (next->job_active && !next->job_started) {
        record_start(next, kt_now32());
    }
    kmpu_switch(next);
    g_k.current = next;
    uint32_t dt = kcycles() - c0;
    g_k.last_sched_cycles = dt;
    if (dt > g_k.max_sched_cycles) {
        g_k.max_sched_cycles = dt;
    }
    kcrit_exit(s);
    return next->sp;
}
