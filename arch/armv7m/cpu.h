/* ARMv7-M helpers shared by the kernel: critical sections, barriers, cycle counter. */
#ifndef KESTREL_ARCH_CPU_H
#define KESTREL_ARCH_CPU_H

#include "board/ra4m1.h"

#include <stdbool.h>
#include <stdint.h>

/* Critical sections mask all configurable interrupts through PRIMASK. They are only used by
 * privileged code and are kept to a few tens of cycles (see docs/ARCHITECTURE.md, ISR rules). */
typedef uint32_t kcrit_t;

#ifdef KESTREL_PROFILE
/* Profiling build: measure every outermost masked interval, keep the longest and the address of
 * the code that ended it (finds sections that delay the kernel timer interrupt). */
extern volatile uint32_t g_crit_t0, g_crit_max, g_crit_max_pc, g_crit_over20us;
static inline kcrit_t kcrit_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (primask == 0u) {
        g_crit_t0 = DWT->CYCCNT;
    }
    return primask;
}

static inline void kcrit_exit(kcrit_t saved)
{
    if (saved == 0u) {
        uint32_t d = DWT->CYCCNT - g_crit_t0;
        if (d > 960u) {
            g_crit_over20us++;
        }
        if (d > g_crit_max && (__get_IPSR() & 0x1FFu) != 16u) { /* ignore the timer ISR itself */
            uint32_t pc;
            __asm volatile("mov %0, pc" : "=r"(pc));
            g_crit_max = d;
            g_crit_max_pc = pc;
        }
    }
    __set_PRIMASK(saved);
}
#else
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
#endif

static inline uint32_t kcycles(void)
{
    return DWT->CYCCNT;
}

static inline bool k_in_isr(void)
{
    return (__get_IPSR() & 0x1FFu) != 0u;
}

/* NVIC priorities (4 implemented bits; lower value = more urgent). */
#define KPRIO_FAULT  0u  /* MemManage, BusFault, UsageFault (fixed priorities) */
#define KPRIO_TIMER  1u  /* kernel timebase + release timer */
#define KPRIO_DEVICE 2u  /* bounded device ISRs (UART, DMA, ADC) */
#define KPRIO_STORM  3u  /* interrupt-storm generator used by the fault campaign */
#define KPRIO_SVC    4u  /* kernel calls from tasks */
#define KPRIO_PENDSV 15u /* context switch: always last */

#endif
