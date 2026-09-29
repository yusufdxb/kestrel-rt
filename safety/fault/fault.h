#ifndef KESTREL_FAULT_H
#define KESTREL_FAULT_H
#include "kernel/kernel_priv.h"

#include <stdint.h>

enum kfault_kind {
    KFAULT_HARD = 1,
    KFAULT_MEMMANAGE = 2,
    KFAULT_BUS = 3,
    KFAULT_USAGE = 4,
    KFAULT_NMI = 5,
    KFAULT_ASSERT = 6,
    KFAULT_TASK_EXIT = 7,
    KFAULT_WATCHDOG_STALL = 8, /* supervisor stopped feeding: detail = stalled-task mask */
    KFAULT_WDT_NMI = 9,
    KFAULT_STACK_CANARY = 11,
    KFAULT_KERNEL = 12,
};

void kfault_init(void);
/* Software-detected faults (assert, task exit): record using the caller's stacked frame. */
void kfault_record_software(ktcb_t *t, const uint32_t *frame, uint16_t kind, uint32_t detail);
/* Record a supervisor stall: frame of the stalled task is reconstructed from its saved context. */
void kfault_record_stall(ktcb_t *t, uint32_t mask);
uint32_t kfault_count(void);
#endif
