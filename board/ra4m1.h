/* Board/MCU access for Kestrel: pulls in the CMSIS-Core + SVD register definitions and nothing else.
 * No FSP BSP code is used; the header only provides register layouts. */
#ifndef KESTREL_BOARD_RA4M1_H
#define KESTREL_BOARD_RA4M1_H

#include <stdint.h>

/* The SVD-generated header leaves the interrupt number enum to the BSP; Kestrel defines it. RA4M1
 * NVIC lines 0..31 are generic and bound to peripheral events through ICU IELSRn. */
typedef enum IRQn {
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn = -13,
    MemoryManagement_IRQn = -12,
    BusFault_IRQn = -11,
    UsageFault_IRQn = -10,
    SVCall_IRQn = -5,
    DebugMonitor_IRQn = -4,
    PendSV_IRQn = -2,
    SysTick_IRQn = -1,
    KESTREL_IRQ0 = 0,
    KESTREL_IRQ31 = 31,
} IRQn_Type;

/* R7FA4M1AB.h includes "system.h" (FSP); Kestrel supplies its own minimal board/system.h. */
#include "R7FA4M1AB.h"

#define KESTREL_ICLK_HZ  48000000u
#define KESTREL_PCLKA_HZ 48000000u
#define KESTREL_PCLKB_HZ 24000000u
#define KESTREL_PCLKD_HZ 48000000u

/* Register protection (PRCR): key 0xA5 in the upper byte. */
#define KESTREL_PRCR_KEY  0xA500u
#define KESTREL_PRCR_PRC0 0x0001u /* clock generation registers */
#define KESTREL_PRCR_PRC1 0x0002u /* low power modes, VBTBKR, MSTPCRA */
#define KESTREL_PRCR_PRC3 0x0008u /* LVD */

static inline void ra_prcr_unlock(uint16_t bits)
{
    R_SYSTEM->PRCR = (uint16_t)(KESTREL_PRCR_KEY | bits);
}

static inline void ra_prcr_lock(void)
{
    R_SYSTEM->PRCR = (uint16_t)KESTREL_PRCR_KEY;
}

#endif
