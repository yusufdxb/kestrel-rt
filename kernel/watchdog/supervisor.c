/* Supervisor: the only code that feeds the hardware watchdog.
 *
 *   critical tasks --kestrel_health_vote()--> last_vote[t]
 *   supervisor (kernel timer path, every 5 ms): all critical votes fresh? --yes--> wdt_feed()
 *                                                                        --no---> record + stop feeding
 * A task that hangs, starves, or detects its own failure stops voting; the watchdog then resets the
 * MCU ~350 ms later. Noncritical tasks (comms, logger) that set a wd_window are only monitored:
 * a stall sets a degraded bit (traced and reported) but never causes a reset. The supervisor runs
 * at timer-interrupt priority, so it keeps running even when every task is starved; only a stuck
 * interrupt-disabled section or a handler-mode hang stops it, and then the WDT bites unaided. */
#include "arch/armv7m/cpu.h"
#include "drivers/wdt/wdt.h"
#include "kernel/kernel_priv.h"
#include "safety/fault/fault.h"
#include "trace/trace.h"

static bool s_withholding;
static uint32_t s_warnings;

uint32_t k_supervisor_warnings(void);

void k_supervisor_init(void)
{
    s_withholding = false;
    (void)wdt_start();
}

uint32_t k_supervisor_warnings(void)
{
    return s_warnings;
}

void k_supervisor_tick(uint64_t now)
{
    uint32_t missing = 0u;
    uint32_t degraded = 0u;
    for (uint32_t i = 0; i < g_k.ntasks; i++) {
        ktcb_t *t = &g_k.tcb[i];
        if (t->wd_window == 0u || now < t->last_vote) {
            continue;
        }
        bool stale = (now - t->last_vote) > t->wd_window || t->state == KS_FAULTED;
        if (!stale) {
            continue;
        }
        if (t->critical) {
            missing |= 1u << i;
        } else {
            degraded |= 1u << i;
        }
    }
    if (degraded != g_k.degraded) {
        g_k.degraded = degraded;
        trace_emit_locked(TE_DEGRADED, 0xFFu, (uint16_t)degraded);
    }
    if (missing == 0u) {
        if (!s_withholding) {
            wdt_feed();
        }
        return;
    }
    if (!s_withholding) {
        s_withholding = true;
        s_warnings++;
        trace_emit_locked(TE_WD_WARNING, 0xFFu, (uint16_t)missing);
        kfault_record_stall(&g_k.tcb[__CLZ(__RBIT(missing))], missing);
    }
}
