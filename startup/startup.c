/* Kestrel startup: vector table and reset handler.
 *
 * Entry contract from the Arduino bootloader (arduino-renesas-bootloader src/main.c, boot5()):
 * PRIMASK = 1, SysTick stopped, VTOR = 0x4000, MSP = vector[0], jump to vector[1]. Clocks are left
 * as the bootloader configured them. Kestrel does not rely on any of that: it re-establishes VTOR,
 * interrupt state, FPU access and clocks itself before running C code that touches .data/.bss.
 */
#include "startup/startup.h"

#include "arch/armv7m/cpu.h"
#include "kernel/kestrel.h"

#include <stdint.h>

extern uint32_t __data_load, __data_start, __data_end;
extern uint32_t __bss_start, __bss_end;
extern uint32_t __task_stacks_start, __task_stacks_end;
extern uint32_t __app_shared_load, __app_shared_start, __app_shared_used_end;
extern uint32_t __main_stack_top;

void kestrel_reset_handler(void) __attribute__((noreturn));
void kestrel_default_handler(void);

/* Handlers provided elsewhere; weak aliases keep an image linkable without every subsystem. */
#define WEAK_DEFAULT __attribute__((weak, alias("kestrel_default_handler")))
void NMI_Handler(void) WEAK_DEFAULT;
void HardFault_Handler(void) WEAK_DEFAULT;
void MemManage_Handler(void) WEAK_DEFAULT;
void BusFault_Handler(void) WEAK_DEFAULT;
void UsageFault_Handler(void) WEAK_DEFAULT;
void SVC_Handler(void) WEAK_DEFAULT;
void DebugMon_Handler(void) WEAK_DEFAULT;
void PendSV_Handler(void) WEAK_DEFAULT;
void SysTick_Handler(void) WEAK_DEFAULT;
void kestrel_irq_dispatch(void) WEAK_DEFAULT;

typedef void (*kvector_fn)(void);

/* 16 system exceptions + 32 ICU-routed interrupts = 48 entries; VTOR alignment requirement is the
 * next power of two of the table size (256 B), satisfied by the 0x4000 origin. */
typedef struct {
    uint32_t *initial_msp;
    kvector_fn system[15];
    kvector_fn irq[KESTREL_NUM_IRQ];
} kvector_table_t;

__attribute__((section(".vectors"), used)) const kvector_table_t kestrel_vectors = {
    .initial_msp = &__main_stack_top,
    .system =
        {
            kestrel_reset_handler,   /* 1  Reset */
            NMI_Handler,             /* 2  NMI */
            HardFault_Handler,       /* 3  HardFault */
            MemManage_Handler,       /* 4  MemManage */
            BusFault_Handler,        /* 5  BusFault */
            UsageFault_Handler,      /* 6  UsageFault */
            0, 0, 0, 0, SVC_Handler, /* 11 SVCall */
            DebugMon_Handler,        /* 12 DebugMonitor */
            0, PendSV_Handler,       /* 14 PendSV */
            SysTick_Handler,         /* 15 SysTick */
        },
    .irq =
        {
#define D8                                                                                         \
    kestrel_irq_dispatch, kestrel_irq_dispatch, kestrel_irq_dispatch, kestrel_irq_dispatch,        \
        kestrel_irq_dispatch, kestrel_irq_dispatch, kestrel_irq_dispatch, kestrel_irq_dispatch
            D8, D8, D8, D8, /* all 32 ICU lines share one dispatcher (drivers/icu) */
#undef D8
        },
};

void kestrel_default_handler(void)
{
    /* An unexpected exception with no handler: stop here so SWD shows the IPSR. The fault module
     * overrides the fault vectors, so this only catches truly unconfigured vectors. */
    __disable_irq();
    for (;;) {
        __BKPT(0);
    }
}

/* Linker symbols are distinct objects to the C abstract machine, so section extents are computed
 * from their addresses as integers instead of comparing pointers to different objects. These
 * helpers must not touch .data/.bss themselves (they run before either is initialized). */
static uint32_t span_words(const uint32_t *start, const uint32_t *end)
{
    return (uint32_t)(((uintptr_t)end - (uintptr_t)start) / 4u);
}

static void copy_words(uint32_t *dst, const uint32_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void zero_words(uint32_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = 0u;
    }
}

static void clear_interrupt_state(void)
{
    /* The bootloader may have enabled or left pending NVIC lines and ICU links. Start clean. */
    for (uint32_t i = 0; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    for (uint32_t i = 0; i < KESTREL_NUM_IRQ; i++) {
        R_ICU->IELSR[i] = 0u;
    }
    for (uint32_t i = 0; i < 4u; i++) {
        R_ICU->DELSR[i] = 0u;
    }
    SysTick->CTRL = 0u;
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
}

void kestrel_reset_handler(void)
{
    __disable_irq();
    SCB->VTOR = (uint32_t)&kestrel_vectors;

    /* Full access to CP10/CP11 before any FP instruction can execute. Lazy FP stacking (FPCCR
     * ASPEN=1, LSPEN=1) is the reset default and is relied on by the context switch. */
    SCB->CPACR |= (0xFu << 20);
    __DSB();
    __ISB();

    copy_words(&__data_start, &__data_load, span_words(&__data_start, &__data_end));
    zero_words(&__bss_start, span_words(&__bss_start, &__bss_end));
    zero_words(&__task_stacks_start, span_words(&__task_stacks_start, &__task_stacks_end));
    copy_words(&__app_shared_start, &__app_shared_load,
               span_words(&__app_shared_start, &__app_shared_used_end));

    clear_interrupt_state();
    kestrel_clock_init();
    kestrel_boot();
}
