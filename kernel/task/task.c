/* Static task table: creation-time validation, initial stack frame, and scheduler start. */
#include "arch/armv7m/cpu.h"
#include "drivers/wdt/wdt.h"
#include "kernel/kernel_priv.h"
#include "kernel/time/timebase.h"
#include "trace/trace.h"

#define XPSR_THUMB       0x01000000u
#define EXC_RETURN_PSP   0xFFFFFFFDu /* thread mode, PSP, basic (no FP) frame */
#define FIRST_RELEASE_US 2000u

void k_task_exit_trap(void);

static ktcb_t s_boot_tcb;
static uint64_t s_boot_stack[64] __attribute__((aligned(8)));

KESTREL_STACK(s_idle_stack, 512);
static volatile uint32_t s_idle_loops;

static void idle_entry(void *arg)
{
    (void)arg;
    /* Busy idle (no WFI): keeps interrupt latency independent of sleep-exit timing and lets
     * CPU utilization be measured as 1 - idle_cycles / elapsed. */
    for (;;) {
        s_idle_loops++;
    }
}

static const kestrel_task_cfg_t s_idle_cfg = {
    "idle", idle_entry, 0, 0u, 0u, 0u, KTASK_PRIVILEGED, s_idle_stack, sizeof(s_idle_stack), 0u,
};

static bool is_pow2(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static void init_tcb(ktcb_t *t, const kestrel_task_cfg_t *cfg, uint8_t id, uint8_t prio)
{
    t->cfg = cfg;
    t->id = id;
    t->base_level = (uint8_t)(2u * prio);
    t->level = t->base_level;
    t->privileged = (cfg->flags & KTASK_PRIVILEGED) != 0u;
    t->critical = (cfg->flags & KTASK_CRITICAL) != 0u;
    t->period = kt_us_to_ticks(cfg->period_us);
    t->deadline = kt_us_to_ticks(cfg->deadline_us);
    t->wd_window = kt_us_to_ticks(cfg->wd_window_us);
    t->wake_at = KNO_TIME;
    t->min_start_lat = 0xFFFFFFFFu;

    kstack_prepare(t);
    t->canary = &cfg->stack_base[KSTACK_CANARY_WORD];
    kmpu_prepare(t);
    uint32_t *top = (uint32_t *)((uintptr_t)cfg->stack_base + cfg->stack_size);
    uint32_t *sp = top - 18;
    /* software-saved part (context_switch.S order): EXC_RETURN, CONTROL, R4..R11 */
    sp[0] = EXC_RETURN_PSP;
    sp[1] = t->privileged ? 0u : 1u; /* CONTROL.nPRIV */
    for (uint32_t i = 2; i < 10; i++) {
        sp[i] = 0u;
    }
    /* hardware exception frame: R0-R3, R12, LR, PC, xPSR */
    sp[10] = (uint32_t)(uintptr_t)cfg->arg;
    sp[11] = 0u;
    sp[12] = 0u;
    sp[13] = 0u;
    sp[14] = 0u;
    sp[15] = (uint32_t)(uintptr_t)k_task_exit_trap;
    sp[16] = (uint32_t)(uintptr_t)cfg->entry & ~1u;
    sp[17] = XPSR_THUMB;
    t->sp = sp;
    g_k.level_owner[t->level] = t;
}

kstatus_t kestrel_task_create(const kestrel_task_cfg_t *cfg)
{
    if (g_k.started || g_k.ntasks >= KESTREL_MAX_TASKS || cfg == 0 || cfg->entry == 0) {
        return KE_INVAL;
    }
    if (cfg->priority < 1u || cfg->priority > KESTREL_MAX_PRIO ||
        g_k.level_owner[2u * cfg->priority] != 0) {
        return KE_INVAL; /* priorities are unique */
    }
    if (!is_pow2(cfg->stack_size) || cfg->stack_size < 256u ||
        ((uintptr_t)cfg->stack_base & (cfg->stack_size - 1u)) != 0u) {
        return KE_INVAL; /* MPU region constraints */
    }
    if (cfg->period_us != 0u && (cfg->deadline_us == 0u || cfg->deadline_us > cfg->period_us)) {
        return KE_INVAL; /* constrained deadlines only: 0 < D <= T */
    }
    if ((cfg->flags & KTASK_CRITICAL) != 0u && cfg->wd_window_us == 0u) {
        return KE_INVAL;
    }
    uint8_t id = (uint8_t)g_k.ntasks++;
    init_tcb(&g_k.tcb[id], cfg, id, cfg->priority);
    g_k.tcb[id].state = KS_BLOCKED;
    g_k.tcb[id].wait = cfg->period_us != 0u ? KW_PERIOD : KW_NONE;
    return KOK;
}

void kestrel_start(void)
{
    __disable_irq();
    ktcb_t *idle = &g_k.tcb[KIDLE_ID];
    init_tcb(idle, &s_idle_cfg, (uint8_t)KIDLE_ID, 0u);
    k_make_ready(idle);

    uint64_t now = kt_now();
    g_k.t0 = now + kt_us_to_ticks(FIRST_RELEASE_US);
    for (uint32_t i = 0; i < g_k.ntasks; i++) {
        ktcb_t *t = &g_k.tcb[i];
        t->last_vote = g_k.t0;
        if (t->period != 0u) {
            t->next_release = g_k.t0; /* synchronous release: the critical instant */
            t->state = KS_BLOCKED;
            t->wait = KW_PERIOD;
        } else {
            k_make_ready(t);
        }
    }
    g_k.next_supervisor = g_k.t0 + kt_us_to_ticks(KSUPERVISOR_PERIOD_US);
    s_boot_tcb.id = 0xFFu;
    s_boot_tcb.state = KS_UNUSED;
    g_k.current = &s_boot_tcb;
    k_supervisor_init();
    kmpu_init();
    g_k.started = true;
    k_recompute_alarm(now);
    k_start_first((uint32_t *)&s_boot_stack[64]);
}

uint32_t kestrel_idle_loops(void);
uint32_t kestrel_idle_loops(void)
{
    return s_idle_loops;
}
