#include "kernel/time/timebase.h"

#include "arch/armv7m/cpu.h"
#include "drivers/icu/icu.h"
#include "kernel/khist.h"

#define MSTPD5_GPT32 (1u << 5)
#define GTCR_CST     1u
/* Minimum lead time for a compare write to be trusted (ticks). If the target is closer than this,
 * the alarm is pended in software instead: 2 us covers the write + readback path. */
#define KT_MIN_LEAD_TICKS (2u * KT_TICKS_PER_US)

static volatile uint32_t s_high;
static volatile uint32_t s_overflows;
static kt_alarm_fn s_alarm;
static volatile uint32_t s_alarm_target;
static volatile bool s_alarm_hw; /* armed in hardware (not software-pended) */

static void ovf_isr(void)
{
    s_high++;
    s_overflows++;
}

volatile uint32_t g_kt_last_entry, g_kt_last_target; /* profiling: last compare ISR */

static void cmp_isr(void)
{
    /* ISR latency: compare-match instant (the programmed target) to this line, in timer ticks.
     * Includes NVIC entry, the ICU dispatcher and any interrupt-masked section in the way. */
    uint32_t cnt = R_GPT0->GTCNT;
    g_kt_last_entry = cnt;
    g_kt_last_target = s_alarm_target;
    if (s_alarm_hw) {
        s_alarm_hw = false;
        khist_add(&g_hist_irqlat, cnt - s_alarm_target);
    }
    if (s_alarm != 0) {
        s_alarm();
    }
}

void kt_init(kt_alarm_fn on_alarm)
{
    s_alarm = on_alarm;
    R_MSTP->MSTPCRD &= ~MSTPD5_GPT32;
    (void)R_MSTP->MSTPCRD;

    R_GPT0->GTCR = 0u;          /* stopped, saw-wave, PCLKD/1 */
    R_GPT0->GTUDDTYC = 1u;      /* count up */
    R_GPT0->GTPR = 0xFFFFFFFFu; /* full 32-bit period */
    R_GPT0->GTCNT = 0u;
    R_GPT0->GTCCR[0] = 0xFFFFFFFFu;
    R_GPT0->GTST = 0u;
    s_high = 0u;

    icu_route(IRQ_LINE_TIMER_OVF, EVT_GPT0_OVF, ovf_isr, KPRIO_TIMER);
    icu_route(IRQ_LINE_TIMER_CMP, EVT_GPT0_CMP_A, cmp_isr, KPRIO_TIMER);
    R_GPT0->GTCR = GTCR_CST;
}

uint64_t kt_now(void)
{
    kcrit_t s = kcrit_enter();
    uint32_t high = s_high;
    uint32_t low = R_GPT0->GTCNT;
    bool pending = icu_is_pending(IRQ_LINE_TIMER_OVF) != 0;
    if (pending) {
        low = R_GPT0->GTCNT; /* re-sample: the wrap is known to have happened */
    }
    kcrit_exit(s);
    return kt_extend(high, low, pending);
}

void kt_alarm_set(uint64_t when)
{
    kt_alarm_set_from(when, kt_now());
}

/* `now` may be slightly stale (the caller's own timestamp): a stale `now` only overstates the
 * lead, and the post-write counter check below still catches a target that already passed. */
void kt_alarm_set_from(uint64_t when, uint64_t now)
{
    kcrit_t s = kcrit_enter();
    int64_t lead = (int64_t)(when - now);
    if (lead > (int64_t)0x7FFFFFFF) {
        when = now + 0x7FFFFFFFu; /* far event: wake halfway and re-evaluate */
        lead = 0x7FFFFFFF;
    }
    if (lead < (int64_t)KT_MIN_LEAD_TICKS) {
        s_alarm_hw = false;
        NVIC_SetPendingIRQ((IRQn_Type)IRQ_LINE_TIMER_CMP);
    } else {
        R_GPT0->GTCCR[0] = (uint32_t)when;
        s_alarm_target = (uint32_t)when;
        s_alarm_hw = true;
        /* Re-check: if the counter passed the target while writing, the match was missed. */
        uint32_t after = R_GPT0->GTCNT;
        if ((int32_t)((uint32_t)when - after) <= 0) {
            s_alarm_hw = false;
            NVIC_SetPendingIRQ((IRQn_Type)IRQ_LINE_TIMER_CMP);
        }
    }
    kcrit_exit(s);
}

void kt_test_preload(uint32_t counter_low, uint32_t high)
{
    kcrit_t s = kcrit_enter();
    R_GPT0->GTCR = 0u;
    R_GPT0->GTCNT = counter_low;
    s_high = high;
    R_GPT0->GTCR = GTCR_CST;
    kcrit_exit(s);
}

uint32_t kt_overflow_count(void)
{
    return s_overflows;
}

/* Diagnostic (privileged, masks interrupts ~25 us): time from compare target to (a) the GPT
 * compare flag GTST.TCFA and (b) the ICU latch IELSR.IR, both polled. Restores the alarm. */
void kt_diag_compare(uint32_t out[4])
{
    kcrit_t s = kcrit_enter();
    uint32_t saved = R_GPT0->GTCCR[0];
    uint32_t target = R_GPT0->GTCNT + 1000u;
    R_GPT0->GTST = 0u;
    R_GPT0->GTCCR[0] = target;
    uint32_t t_flag = 0u, t_ir = 0u;
    for (uint32_t i = 0; i < 100000u && (t_flag == 0u || t_ir == 0u); i++) {
        uint32_t now = R_GPT0->GTCNT;
        if (t_flag == 0u && (R_GPT0->GTST & 1u) != 0u) {
            t_flag = now;
        }
        if (t_ir == 0u && icu_is_pending(IRQ_LINE_TIMER_CMP) != 0) {
            t_ir = now;
        }
    }
    out[0] = t_flag - target;
    out[1] = t_ir - target;
    out[2] = R_GPT0->GTCR;
    out[3] = R_GPT0->GTIOR;
    R_ICU->IELSR[IRQ_LINE_TIMER_CMP] &= ~(1u << 16);
    NVIC_ClearPendingIRQ((IRQn_Type)IRQ_LINE_TIMER_CMP);
    R_GPT0->GTCCR[0] = saved;
    kcrit_exit(s);
    NVIC_SetPendingIRQ((IRQn_Type)IRQ_LINE_TIMER_CMP); /* re-evaluate kernel alarms */
}

/* Diagnostic: DWT cycles for 1000 iterations of (a) a register-only loop, (b) an SRAM load loop,
 * (c) a GPT register read loop, (d) an ICU register read loop. Interrupts masked. */
void kt_diag_cpu(uint32_t out[4])
{
    static volatile uint32_t ram_word;
    kcrit_t s = kcrit_enter();
    uint32_t c0 = kcycles();
    __asm volatile("mov r0, #1000\n1: subs r0, #1\n bne 1b" ::: "r0", "cc");
    uint32_t c1 = kcycles();
    for (uint32_t i = 0; i < 1000u; i++) {
        (void)ram_word;
    }
    uint32_t c2 = kcycles();
    for (uint32_t i = 0; i < 1000u; i++) {
        (void)R_GPT0->GTCNT;
    }
    uint32_t c3 = kcycles();
    for (uint32_t i = 0; i < 1000u; i++) {
        (void)R_ICU->IELSR[0];
    }
    uint32_t c4 = kcycles();
    kcrit_exit(s);
    (void)c4;
    out[0] = c1 - c0;
    out[1] = c2 - c1;
    out[2] = c3 - c2;
    out[3] = c4 - c3;
}
