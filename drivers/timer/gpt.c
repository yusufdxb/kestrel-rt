#include "drivers/timer/gpt.h"

#include "arch/armv7m/cpu.h"
#include "drivers/icu/icu.h"

static volatile uint32_t s_count;
static bool s_routed;

static void storm_isr(void)
{
    s_count++;
}

void storm_set_rate(uint32_t hz)
{
    R_GPT1->GTCR = 0u;
    if (hz == 0u) {
        return;
    }
    if (!s_routed) {
        R_MSTP->MSTPCRD &= ~(1u << 5);
        icu_route(IRQ_LINE_STORM, EVT_GPT1_OVF, storm_isr, KPRIO_STORM);
        s_routed = true;
    }
    R_GPT1->GTUDDTYC = 1u;
    R_GPT1->GTPR = (KESTREL_PCLKD_HZ / hz) - 1u;
    R_GPT1->GTCNT = 0u;
    R_GPT1->GTCR = 1u;
}

uint32_t storm_count(void)
{
    return s_count;
}
