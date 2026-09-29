/* Kernel boot: runs once after the C runtime is up, before the application's kestrel_main(). */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"
#include "kernel/time/timebase.h"
#include "safety/crashlog/crashlog.h"
#include "safety/fault/fault.h"
#include "startup/startup.h"
#include "trace/telemetry.h"
#include "trace/trace.h"

static void dwt_enable(void)
{
    /* Kestrel owns the cycle counter. A debugger detach clears DEMCR.TRCENA (observed with pyOCD
     * on this board), so the kernel re-arms it from the timer path as well (k_dwt_guard). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *(volatile uint32_t *)0xE0001FB0u = 0xC5ACCE55u; /* DWT_LAR unlock (no effect if absent) */
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void k_dwt_guard(void)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0u ||
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        g_k.dwt_rearms++;
    }
}

bool kestrel_dwt_ok(void)
{
    uint32_t a = DWT->CYCCNT;
    for (volatile int i = 0; i < 8; i++) {
    }
    return DWT->CYCCNT != a;
}

void kestrel_boot(void)
{
    dwt_enable();
    crashlog_boot();
    kfault_init();
    trace_init();
    kt_init(k_alarm_isr);
    telemetry_init();
    __enable_irq();
    trace_emit(TE_RESET_REASON, 0xFFu, (uint16_t)crashlog_info()->reset_cause);
    kestrel_main();
    /* kestrel_main must end in kestrel_start(); reaching here is a configuration error. */
    for (;;) {
        __BKPT(1);
    }
}
