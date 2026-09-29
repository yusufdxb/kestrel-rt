#include "apps/fault_injection/inject.h"

#include "kernel/kestrel.h"

KESTREL_SHARED inject_req_t g_inject;
KESTREL_SHARED volatile uint32_t g_inject_sink;

extern uint32_t __bss_start; /* start of kernel .bss: privileged-only memory */

/* Bad addresses come from a volatile so the compiler cannot prove (and reject) the fault. */
static volatile uintptr_t s_bad_addr[3] = {0x0u, 0x10u, 0x70000000u};
KESTREL_SHARED static volatile uint32_t s_keep_going = 1u;

static uint32_t __attribute__((noinline)) recurse(uint32_t depth)
{
    volatile uint32_t pad[16];
    pad[0] = depth;
    pad[15] = depth;
    if (s_keep_going == 0u) {
        return pad[0];
    }
    return recurse(depth + 1u) + pad[0] + pad[15];
}

void inject_check(uint8_t task_id)
{
    if (g_inject.kind == INJ_NONE || g_inject.task != task_id) {
        return;
    }
    uint8_t kind = g_inject.kind;
    uint16_t arg = g_inject.arg;
    g_inject.kind = INJ_NONE;
    kestrel_trace((uint16_t)(0xF000u | kind));
    switch (kind) {
    case INJ_NULL_READ:
        g_inject_sink = *(volatile uint32_t *)s_bad_addr[0];
        break;
    case INJ_NULL_WRITE:
        *(volatile uint32_t *)s_bad_addr[1] = 0xDEADu;
        break;
    case INJ_ILLEGAL_INSN:
        __asm volatile(".short 0xDE00"); /* UDF #0 */
        break;
    case INJ_DIV_ZERO: {
        volatile uint32_t zero = 0u;
        g_inject_sink = 100u / zero;
        break;
    }
    case INJ_UNMAPPED_READ:
        g_inject_sink = *(volatile uint32_t *)s_bad_addr[2];
        break;
    case INJ_KERNEL_WRITE:
        *(volatile uint32_t *)&__bss_start = 0xBADu;
        break;
    case INJ_STACK_OVERFLOW:
        g_inject_sink = recurse(0u);
        break;
    case INJ_ASSERT:
        KESTREL_ASSERT(arg == 0xFFFFu);
        break;
    case INJ_HANG:
        for (;;) {
            g_inject_sink++;
        }
    case INJ_EXEC_RAM: {
        void (*fn)(void) = (void (*)(void))(((uintptr_t)&g_inject_sink) | 1u);
        fn();
        break;
    }
    case INJ_SPIN_MS: {
        uint64_t end = kestrel_now() + (uint64_t)arg * 48000u;
        while (kestrel_now() < end) {
        }
        break;
    }
    default:
        break;
    }
}
