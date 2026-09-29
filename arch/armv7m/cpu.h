/* ARMv7-M helpers shared by the kernel: critical sections, barriers, cycle counter. */
#ifndef KESTREL_ARCH_CPU_H
#define KESTREL_ARCH_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "board/ra4m1.h"

/* Critical sections mask all configurable interrupts through PRIMASK. They are only used by
 * privileged code and are kept to a few tens of cycles (see docs/ARCHITECTURE.md, ISR rules). */
typedef uint32_t kcrit_t;

static inline kcrit_t kcrit_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void kcrit_exit(kcrit_t saved)
{
    __set_PRIMASK(saved);
}

static inline uint32_t kcycles(void)
{
    return DWT->CYCCNT;
}

static inline bool k_in_isr(void)
{
    return (__get_IPSR() & 0x1FFu) != 0u;
}

/* NVIC priorities (4 implemented bits; lower value = more urgent). */
#define KPRIO_FAULT   0u  /* MemManage, BusFault, UsageFault (fixed priorities) */
#define KPRIO_TIMER   1u  /* kernel timebase + release timer */
#define KPRIO_DEVICE  2u  /* bounded device ISRs (UART, DMA, ADC) */
#define KPRIO_STORM   3u  /* interrupt-storm generator used by the fault campaign */
#define KPRIO_SVC     4u  /* kernel calls from tasks */
#define KPRIO_PENDSV  15u /* context switch: always last */

#endif
