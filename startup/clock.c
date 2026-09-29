/* RA4M1 clock setup, owned by Kestrel (no FSP BSP).
 *
 * Order matters (RA4M1 User's Manual, clock generation circuit and operating power control mode):
 *   1. high-speed operating power mode (required for ICLK > 12 MHz range limits),
 *   2. HOCO running and stable (frequency 48 MHz is selected by OFS1.HOCOFRQ1, programmed by the
 *      factory/bootloader option bytes; Kestrel verifies the result by measurement, not assumption),
 *   3. one flash wait state (MEMWAIT = 1) before ICLK exceeds 32 MHz,
 *   4. dividers, then 5. clock source switch.
 * Re-applying this on top of the bootloader's identical configuration is harmless.
 */
#include "arch/armv7m/cpu.h"
#include "startup/startup.h"

void kestrel_fcache(uint32_t on);

#define SCKDIVCR_FCK_DIV2  (1u << 28)
#define SCKDIVCR_ICK_DIV1  (0u << 24)
#define SCKDIVCR_PCKA_DIV1 (0u << 12)
#define SCKDIVCR_PCKB_DIV2 (1u << 8)
#define SCKDIVCR_PCKC_DIV1 (0u << 4)
#define SCKDIVCR_PCKD_DIV1 (0u << 0)
#define KESTREL_SCKDIVCR                                                                           \
    (SCKDIVCR_FCK_DIV2 | SCKDIVCR_ICK_DIV1 | SCKDIVCR_PCKA_DIV1 | SCKDIVCR_PCKB_DIV2 |             \
     SCKDIVCR_PCKC_DIV1 | SCKDIVCR_PCKD_DIV1)
#define SCKSCR_HOCO 0u

void kestrel_clock_init(void)
{
    ra_prcr_unlock(KESTREL_PRCR_PRC0 | KESTREL_PRCR_PRC1);

    /* 1. High-speed mode: OPCM = 0. Wait for any transition in progress before and after. */
    while ((R_SYSTEM->OPCCR & 0x10u) != 0u) {
    }
    R_SYSTEM->OPCCR = 0u;
    while ((R_SYSTEM->OPCCR & 0x10u) != 0u) {
    }

    /* 2. HOCO on, wait for the stabilization flag. */
    R_SYSTEM->HOCOCR = 0u;
    while ((R_SYSTEM->OSCSF & 0x01u) == 0u) {
    }

    /* 3. Flash wait state before running above 32 MHz. */
    R_SYSTEM->MEMWAIT = 1u;

    /* 4./5. Dividers and source. */
    R_SYSTEM->SCKDIVCR = KESTREL_SCKDIVCR;
    R_SYSTEM->SCKSCR = SCKSCR_HOCO;

    ra_prcr_lock();
    kestrel_fcache(1u);
}

void kestrel_clock_read(kestrel_clock_state_t *out)
{
    out->sckdivcr = R_SYSTEM->SCKDIVCR;
    out->sckscr = R_SYSTEM->SCKSCR;
    out->memwait = R_SYSTEM->MEMWAIT;
    out->opccr = R_SYSTEM->OPCCR;
    out->hocosf = (uint8_t)(R_SYSTEM->OSCSF & 1u);
}

/* Flash cache: enabled by Kestrel itself (never relies on the bootloader having done it).
 * Invalidate first so no stale lines survive a reprogram. */
void kestrel_fcache(uint32_t on)
{
    R_FCACHE->FCACHEE = 0u;
    if (on != 0u) {
        R_FCACHE->FCACHEIV = 1u;
        while (R_FCACHE->FCACHEIV != 0u) {
        }
        R_FCACHE->FCACHEE = 1u;
    }
}
