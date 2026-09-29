#include "drivers/icu/icu.h"

#include "arch/armv7m/cpu.h"
#include "startup/startup.h"

#include <stdbool.h>

#define IELSR_IR (1u << 16)

static kestrel_isr_t s_isr[KESTREL_NUM_IRQ];
static uint32_t s_level_lines; /* bit n: line n clears IR after its handler */
volatile uint32_t icu_last_entry_cycles[IRQ_LINE_COUNT];

void kestrel_irq_dispatch(void);

void icu_route_mode(uint32_t line, uint32_t event, kestrel_isr_t isr, uint32_t prio, uint32_t mode)
{
    if (line >= KESTREL_NUM_IRQ) {
        return;
    }
    if (mode == ICU_LEVEL) {
        s_level_lines |= 1u << line;
    } else {
        s_level_lines &= ~(1u << line);
    }
    NVIC_DisableIRQ((IRQn_Type)line);
    s_isr[line] = isr;
    R_ICU->IELSR[line] = event;
    NVIC_ClearPendingIRQ((IRQn_Type)line);
    NVIC_SetPriority((IRQn_Type)line, prio);
    NVIC_EnableIRQ((IRQn_Type)line);
}

void icu_disable(uint32_t line)
{
    if (line >= KESTREL_NUM_IRQ) {
        return;
    }
    NVIC_DisableIRQ((IRQn_Type)line);
    R_ICU->IELSR[line] = 0u;
    s_isr[line] = 0;
}

int icu_is_pending(uint32_t line)
{
    return (R_ICU->IELSR[line] & IELSR_IR) != 0u;
}

/* Single dispatcher for all ICU lines. The ICU latches events in IELSRn.IR; it must be cleared by
 * software before the handler runs (and read back so the write lands before exception return),
 * otherwise the line re-enters immediately. */
void kestrel_irq_dispatch(void)
{
    uint32_t cyc = kcycles();
    uint32_t line = ((__get_IPSR() & 0x1FFu) - 16u) & (KESTREL_NUM_IRQ - 1u);
    bool level = (s_level_lines & (1u << line)) != 0u;
    if (!level) {
        R_ICU->IELSR[line] &= ~IELSR_IR;
        (void)R_ICU->IELSR[line];
    }
    if (line < IRQ_LINE_COUNT) {
        icu_last_entry_cycles[line] = cyc;
    }
    kestrel_isr_t isr = s_isr[line];
    if (isr != 0) {
        isr();
    }
    if (level) {
        R_ICU->IELSR[line] &= ~IELSR_IR;
        (void)R_ICU->IELSR[line]; /* read back: the clear must land before exception return */
    }
}
