/* Cortex-M fault capture and containment.
 *
 * Every fault produces a crash record in the VBATT backup registers (see crashlog.h). Policy:
 *   - fault in handler mode (kernel, ISR) or in a privileged system task: record, reset;
 *   - fault in a CRITICAL task: record, reset (the control loop cannot continue safely);
 *   - fault in a noncritical task: record, mark the task FAULTED, switch away, keep running.
 * The MPU is switched off while the record is captured so a stack-overflow frame that ends in a
 * guard region can still be read; it is switched back on before returning. */
#include "safety/fault/fault.h"

#include "arch/armv7m/cpu.h"
#include "kernel/time/timebase.h"
#include "safety/crashlog/crashlog.h"
#include "trace/trace.h"

#define CFSR_MSTKERR (1u << 4)
#define CFSR_STKERR  (1u << 12)
#define CFSR_MMARVALID (1u << 7)
#define CFSR_BFARVALID (1u << 15)

void kfault_dispatch(uint32_t *frame, uint32_t exc_return, uint32_t ipsr, uint32_t entry_cycles);

static uint32_t s_faults;
static uint64_t s_scratch_psp[32] __attribute__((aligned(8)));

void kfault_init(void)
{
    /* Separate handlers for MemManage/BusFault/UsageFault (instead of escalating to HardFault);
     * divide-by-zero traps (UsageFault). Unaligned access stays allowed: the compiler relies on it
     * for packed data, trapping it would turn correct code into faults. */
    NVIC_SetPriority(MemoryManagement_IRQn, KPRIO_FAULT);
    NVIC_SetPriority(BusFault_IRQn, KPRIO_FAULT);
    NVIC_SetPriority(UsageFault_IRQn, KPRIO_FAULT);
    NVIC_SetPriority(SVCall_IRQn, KPRIO_SVC);
    NVIC_SetPriority(PendSV_IRQn, KPRIO_PENDSV);
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
    SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
}

uint32_t kfault_count(void)
{
    return s_faults;
}

static void fill_system(kcrash_record_t *r)
{
    r->cfsr = SCB->CFSR;
    r->hfsr = SCB->HFSR;
    r->mmfar = SCB->MMFAR;
    r->bfar = SCB->BFAR;
    r->shcsr = SCB->SHCSR;
    r->icsr = SCB->ICSR;
    uint64_t now = kt_now();
    r->time_lo = (uint32_t)now;
    r->time_hi = (uint32_t)(now >> 32);
}

static void fill_frame(kcrash_record_t *r, const uint32_t *f)
{
    r->r0 = f[0];
    r->r1 = f[1];
    r->r2 = f[2];
    r->r3 = f[3];
    r->r12 = f[4];
    r->lr = f[5];
    r->pc = f[6];
    r->xpsr = f[7];
    r->sp = (uint32_t)(uintptr_t)f;
    r->flags |= KCF_FRAME_VALID;
}

static void clear_cfsr(void)
{
    SCB->CFSR = SCB->CFSR; /* write-one-to-clear */
    SCB->HFSR = SCB->HFSR;
}

static bool frame_readable(const uint32_t *f)
{
    uintptr_t a = (uintptr_t)f;
    return a >= 0x20000000u && a + 32u <= 0x20008000u && (a & 3u) == 0u;
}

static void __attribute__((noreturn)) fatal_reset(void)
{
    __DSB();
    NVIC_SystemReset();
}

void kfault_dispatch(uint32_t *frame, uint32_t exc_return, uint32_t ipsr, uint32_t entry_cycles)
{
    uint32_t mpu_ctrl = MPU->CTRL;
    MPU->CTRL = 0u;
    __DSB();
    __ISB();
    s_faults++;

    kcrash_record_t r;
    for (uint32_t i = 0; i < sizeof(r) / 4u; i++) {
        ((uint32_t *)&r)[i] = 0u;
    }
    uint32_t exc = ipsr & 0x1FFu;
    r.kind = (uint16_t)(exc == 2u ? KFAULT_NMI
                        : exc == 3u ? KFAULT_HARD
                        : exc == 4u ? KFAULT_MEMMANAGE
                        : exc == 5u ? KFAULT_BUS
                                    : KFAULT_USAGE);
    r.exc_return = exc_return;
    fill_system(&r);
    bool thread_psp = (exc_return & 0x4u) != 0u;
    ktcb_t *t = g_k.started ? g_k.current : 0;
    r.task = (thread_psp && t != 0) ? t->id : 0xFFu;
    if ((r.cfsr & (CFSR_MSTKERR | CFSR_STKERR)) != 0u) {
        r.flags |= KCF_STACK_ERR;
    }
    if (frame_readable(frame)) {
        fill_frame(&r, frame);
    } else {
        r.sp = (uint32_t)(uintptr_t)frame;
    }
    if (!thread_psp) {
        r.flags |= KCF_IN_HANDLER;
    }
    if (r.task != 0xFFu && t->cfg != 0) {
        r.stack_used = (uint16_t)kstack_high_water(t);
    }
    bool contain = thread_psp && t != 0 && t->cfg != 0 && !t->critical && !t->privileged &&
                   r.kind != KFAULT_HARD && r.kind != KFAULT_NMI;
    if (!contain) {
        r.flags |= KCF_RESET;
    }
    uint32_t dt = kcycles() - entry_cycles;
    r.capture_cycles = (uint16_t)(dt > 0xFFFFu ? 0xFFFFu : dt);
    crashlog_store(&r);
    clear_cfsr();

    if (!contain) {
        fatal_reset();
    }
    /* Contain: park the faulted task, give PendSV a harmless PSP to save into, switch away. */
    kcrit_t s = kcrit_enter();
    t->faults++;
    trace_emit_locked(TE_FAULT, t->id, r.kind);
    k_task_fault(t, r.kind);
    __set_PSP((uint32_t)(uintptr_t)&s_scratch_psp[32]);
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    kcrit_exit(s);
    MPU->CTRL = mpu_ctrl;
    __DSB();
    __ISB();
}

void kfault_record_software(ktcb_t *t, const uint32_t *frame, uint16_t kind, uint32_t detail)
{
    kcrash_record_t r;
    for (uint32_t i = 0; i < sizeof(r) / 4u; i++) {
        ((uint32_t *)&r)[i] = 0u;
    }
    uint32_t c0 = kcycles();
    r.kind = kind;
    r.task = t->id;
    r.detail = detail;
    fill_system(&r);
    fill_frame(&r, frame);
    r.stack_used = (uint16_t)kstack_high_water(t);
    if (t->critical || t->privileged) {
        r.flags |= KCF_RESET;
    }
    uint32_t dt = kcycles() - c0;
    r.capture_cycles = (uint16_t)(dt > 0xFFFFu ? 0xFFFFu : dt);
    crashlog_store(&r);
    s_faults++;
}

/* Reconstruct the stalled task's PC from its saved context (or its live frame if running). */
void kfault_record_stall(ktcb_t *t, uint32_t mask)
{
    kcrash_record_t r;
    for (uint32_t i = 0; i < sizeof(r) / 4u; i++) {
        ((uint32_t *)&r)[i] = 0u;
    }
    r.kind = KFAULT_WATCHDOG_STALL;
    r.task = t->id;
    r.detail = mask;
    fill_system(&r);
    const uint32_t *hw;
    if (t == g_k.current) {
        hw = (const uint32_t *)__get_PSP(); /* the timer ISR interrupted this task */
    } else {
        const uint32_t *sw = t->sp;
        hw = sw + 10u + (((sw[0] & 0x10u) == 0u) ? 16u : 0u);
    }
    if (frame_readable(hw)) {
        fill_frame(&r, hw);
    }
    r.stack_used = (uint16_t)kstack_high_water(t);
    r.flags |= KCF_RESET;
    crashlog_store(&r);
}

void k_task_fault(ktcb_t *t, uint16_t kind)
{
    if (t->critical || t->privileged) {
        fatal_reset();
    }
    t->state = KS_FAULTED;
    t->job_active = false;
    g_k.ready_levels &= ~(1u << t->level);
    trace_emit_locked(TE_FAULT, t->id, kind);
}
