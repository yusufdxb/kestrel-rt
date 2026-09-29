#include "kernel/time/timebase.h"

#include "arch/armv7m/cpu.h"
#include "drivers/icu/icu.h"

#define MSTPD5_GPT32 (1u << 5)
#define GTCR_CST     1u
/* Minimum lead time for a compare write to be trusted (ticks). If the target is closer than this,
 * the alarm is pended in software instead: 2 us covers the write + readback path. */
#define KT_MIN_LEAD_TICKS (2u * KT_TICKS_PER_US)

static volatile uint32_t s_high;
static volatile uint32_t s_overflows;
static kt_alarm_fn s_alarm;

static void ovf_isr(void)
{
    s_high++;
    s_overflows++;
}

static void cmp_isr(void)
{
    if (s_alarm != 0) {
        s_alarm();
    }
}

void kt_init(kt_alarm_fn on_alarm)
{
    s_alarm = on_alarm;
    R_MSTP->MSTPCRD &= ~MSTPD5_GPT32;
    (void)R_MSTP->MSTPCRD;

    R_GPT0->GTCR = 0u;           /* stopped, saw-wave, PCLKD/1 */
    R_GPT0->GTUDDTYC = 1u;       /* count up */
    R_GPT0->GTPR = 0xFFFFFFFFu;  /* full 32-bit period */
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
    kcrit_t s = kcrit_enter();
    uint64_t now = kt_now();
    int64_t lead = (int64_t)(when - now);
    if (lead > (int64_t)0x7FFFFFFF) {
        when = now + 0x7FFFFFFFu; /* far event: wake halfway and re-evaluate */
        lead = 0x7FFFFFFF;
    }
    if (lead < (int64_t)KT_MIN_LEAD_TICKS) {
        NVIC_SetPendingIRQ((IRQn_Type)IRQ_LINE_TIMER_CMP);
    } else {
        R_GPT0->GTCCR[0] = (uint32_t)when;
        /* Re-check: if the counter passed the target while writing, the match was missed. */
        uint32_t after = R_GPT0->GTCNT;
        if ((int32_t)((uint32_t)when - after) <= 0) {
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
