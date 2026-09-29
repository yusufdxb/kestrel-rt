/* Cortex-M4 MPU policy (8 regions; a higher region number wins where regions overlap).
 *
 *  #  base                 size   access (priv / unpriv)   XN  purpose
 *  0  0x00000000           256K   RO / RO                  no  code + constants
 *  1  0x00000000           16K    none / none              yes bootloader block: NULL-pointer trap
 *  2  0x20000000           32K    RW / none                yes kernel data (and everything else)
 *  3  __app_shared_start   2K     RW / RW                  yes application shared data (IPC)
 *  4  current task stack   var    RW / RW                  yes switched on every context switch
 *  5  current stack bottom 32B    none / none              yes task stack guard
 *  6  main stack bottom    32B    none / none              yes MSP guard (ISR/kernel overflow)
 *  7  0x40078000 (GPT320)  256B   RW / RO                  yes timebase readable by tasks
 * PRIVDEFENA = 1: privileged code sees the default map elsewhere (peripherals, PPB).
 * HFNMIENA = 0: the MPU is off while HardFault/NMI run, so fault capture can read any stack.
 * No execution from SRAM at all (regions 2-6 are XN): data cannot be run as code. */
#include "arch/armv7m/cpu.h"
#include "kernel/kernel_priv.h"

extern uint32_t __app_shared_start, __app_shared_end, __main_stack_bottom;

#define AP_NONE           (0u << MPU_RASR_AP_Pos)
#define AP_PRIV_RW        (1u << MPU_RASR_AP_Pos)
#define AP_URO            (2u << MPU_RASR_AP_Pos) /* priv RW, unpriv RO */
#define AP_FULL           (3u << MPU_RASR_AP_Pos)
#define AP_RO             (6u << MPU_RASR_AP_Pos) /* priv RO, unpriv RO */
#define XN                MPU_RASR_XN_Msk
#define MEM_NORMAL        ((1u << MPU_RASR_C_Pos)) /* TEX 0, C 1, B 0 */
#define MEM_SRAM          ((1u << MPU_RASR_C_Pos) | (1u << MPU_RASR_S_Pos))
#define MEM_DEVICE        ((1u << MPU_RASR_B_Pos) | (1u << MPU_RASR_S_Pos))
#define STACK_GUARD_BYTES 32u

static bool s_enabled;

static uint32_t size_field(uint32_t bytes)
{
    /* RASR.SIZE = log2(bytes) - 1; bytes is a power of two >= 32 */
    return ((31u - __CLZ(bytes)) - 1u) << MPU_RASR_SIZE_Pos;
}

static void region(uint32_t n, uint32_t base, uint32_t bytes, uint32_t attrs)
{
    MPU->RNR = n;
    MPU->RBAR = base;
    MPU->RASR = attrs | size_field(bytes) | MPU_RASR_ENABLE_Msk;
}

#ifndef KESTREL_CFG_MPU
#define KESTREL_CFG_MPU 1
#endif

void kmpu_init(void)
{
    if (KESTREL_CFG_MPU == 0) {
        s_enabled = false; /* like-for-like comparisons with non-MPU kernels */
        return;
    }
    uint32_t shared = (uint32_t)(uintptr_t)&__app_shared_start;
    uint32_t shared_size =
        (uint32_t)((uintptr_t)&__app_shared_end - (uintptr_t)&__app_shared_start);
    MPU->CTRL = 0u;
    region(0u, 0x00000000u, 256u * 1024u, AP_RO | MEM_NORMAL);
    region(1u, 0x00000000u, 16u * 1024u, AP_NONE | XN | MEM_NORMAL);
    region(2u, 0x20000000u, 32u * 1024u, AP_PRIV_RW | XN | MEM_SRAM);
    region(3u, shared, shared_size, AP_FULL | XN | MEM_SRAM);
    region(6u, (uint32_t)(uintptr_t)&__main_stack_bottom, STACK_GUARD_BYTES,
           AP_NONE | XN | MEM_SRAM);
    region(7u, 0x40078000u, 256u, AP_URO | XN | MEM_DEVICE);
    /* regions 4/5 are programmed per task by kmpu_switch */
    MPU->RNR = 4u;
    MPU->RASR = 0u;
    MPU->RNR = 5u;
    MPU->RASR = 0u;
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();
    s_enabled = true;
}

bool kmpu_enabled(void)
{
    return s_enabled;
}

void kmpu_switch(const ktcb_t *t)
{
    if (!s_enabled || t->cfg == 0) {
        return;
    }
    uint32_t base = (uint32_t)(uintptr_t)t->cfg->stack_base;
    /* Two register writes per region (RBAR with VALID|REGION selects the region itself). */
    MPU->RBAR = base | MPU_RBAR_VALID_Msk | 4u;
    MPU->RASR = AP_FULL | XN | MEM_SRAM | size_field(t->cfg->stack_size) | MPU_RASR_ENABLE_Msk;
    MPU->RBAR = base | MPU_RBAR_VALID_Msk | 5u;
    MPU->RASR = AP_NONE | XN | MEM_SRAM | size_field(STACK_GUARD_BYTES) | MPU_RASR_ENABLE_Msk;
    /* No barrier needed here: the exception return that follows is context synchronizing. */
}

void kmpu_set_enabled(bool on)
{
    __disable_irq();
    if (on) {
        MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    } else {
        MPU->CTRL = 0u;
    }
    s_enabled = on;
    __DSB();
    __ISB();
    __enable_irq();
}
