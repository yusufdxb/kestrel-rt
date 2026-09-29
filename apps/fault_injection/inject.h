/* Deliberate fault injection, requested by the host and executed by the targeted task at the start
 * of its next job (a defined safe point), never asynchronously. */
#ifndef KESTREL_INJECT_H
#define KESTREL_INJECT_H
#include <stdint.h>

enum inject_kind {
    INJ_NONE = 0,
    INJ_NULL_READ = 1,
    INJ_NULL_WRITE = 2,
    INJ_ILLEGAL_INSN = 3,
    INJ_DIV_ZERO = 4,
    INJ_UNMAPPED_READ = 5,  /* 0x70000000: no memory there */
    INJ_KERNEL_WRITE = 6,   /* write into kernel data: MPU must stop an unprivileged task */
    INJ_STACK_OVERFLOW = 7, /* unbounded recursion into the stack guard */
    INJ_ASSERT = 8,
    INJ_HANG = 9,      /* infinite loop: no votes, no job completion */
    INJ_EXEC_RAM = 10, /* jump into SRAM: XN must stop it */
    INJ_RETURN = 11,   /* task entry returns */
    INJ_SPIN_MS = 12,  /* busy-wait arg ms inside one job (overload one job) */
};

typedef struct {
    volatile uint8_t kind;
    volatile uint8_t task;
    volatile uint16_t arg;
} inject_req_t;

extern inject_req_t g_inject;
/* Called by a task at job start; performs the injection if it targets `task_id`. Returns
 * INJ_RETURN when the task must return from its entry function (that case needs the caller). */
uint8_t inject_check(uint8_t task_id);
#endif
