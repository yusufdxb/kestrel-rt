/* Task stack engineering: fill pattern for high-water measurement, a canary word just above the
 * 32-byte MPU guard block, and the measurement itself.
 *
 *   stack_base                                                     stack_base + size
 *   | guard 32 B (MPU no-access) | canary | fill ... used <- sp  | initial frame |
 */
#include "kernel/kernel_priv.h"

void kstack_prepare(ktcb_t *t)
{
    uint32_t *base = t->cfg->stack_base;
    uint32_t words = t->cfg->stack_size / 4u;
    for (uint32_t i = 0; i < words; i++) {
        base[i] = KSTACK_FILL;
    }
    base[KSTACK_GUARD_WORDS] = KSTACK_CANARY;
}

bool kstack_canary_ok(const ktcb_t *t)
{
    return t->cfg->stack_base[KSTACK_GUARD_WORDS] == KSTACK_CANARY;
}

uint32_t kstack_high_water(const ktcb_t *t)
{
    const uint32_t *base = t->cfg->stack_base;
    uint32_t words = t->cfg->stack_size / 4u;
    uint32_t i = KSTACK_GUARD_WORDS + 1u;
    while (i < words && base[i] == KSTACK_FILL) {
        i++;
    }
    return (words - i) * 4u;
}
