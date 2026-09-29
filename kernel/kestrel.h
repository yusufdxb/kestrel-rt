/* Kestrel-RT public API.
 *
 * Task code (privileged or not) calls the kestrel_* functions below; each one is a supervisor call,
 * so the kernel state they touch stays inside privileged memory. Kernel-internal code uses the
 * k_* functions in kernel/kernel_priv.h instead. */
#ifndef KESTREL_H
#define KESTREL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KESTREL_VERSION "0.1.0"

/* ---- configuration limits (docs/SPEC.md section 3) ---- */
#define KESTREL_MAX_TASKS 8u /* application tasks; the kernel idle task is extra */
#define KESTREL_MAX_PRIO  7u /* application priorities 1..7, unique; 0 is idle */

/* ---- task definition ---- */
#define KTASK_PRIVILEGED 0x01u /* runs privileged (system service); default is unprivileged */
#define KTASK_CRITICAL   0x02u /* must vote to the supervisor; fault => system reset */
#define KTASK_USES_FPU   0x04u /* documentation only: FP context is saved lazily for everyone */

typedef struct {
    const char *name;
    void (*entry)(void *arg);
    void *arg;
    uint32_t period_us;    /* 0 => aperiodic (event-driven or best effort) */
    uint32_t deadline_us;  /* relative deadline, 0 < D <= T; 0 for aperiodic */
    uint8_t priority;      /* 1..KESTREL_MAX_PRIO, unique, higher value runs first */
    uint8_t flags;         /* KTASK_* */
    uint32_t *stack_base;  /* from KESTREL_STACK(): power-of-two size, aligned to its size */
    uint32_t stack_size;   /* bytes, power of two, >= 256 */
    uint32_t wd_window_us; /* KTASK_CRITICAL: longest allowed gap between health votes */
} kestrel_task_cfg_t;

/* Stack storage: power-of-two size, naturally aligned so one MPU region covers it exactly. */
#define KESTREL_STACK(name, bytes)                                                                 \
    _Static_assert(((bytes) & ((bytes)-1)) == 0 && (bytes) >= 256, "stack size: power of two");    \
    static uint32_t name[(bytes) / 4u] __attribute__((aligned(bytes), section(".task_stacks")))

/* Data an unprivileged task may touch must live in the shared application region. */
#define KESTREL_SHARED __attribute__((section(".app_shared")))

/* ---- application entry points (replaces Arduino setup()/loop()) ---- */
void kestrel_main(void);

/* Boot-time API (privileged, before kestrel_start). */
typedef enum {
    KOK = 0,
    KE_INVAL = -1,
    KE_FULL = -2,
    KE_TIMEOUT = -3,
    KE_STATE = -4,
    KE_PERM = -5,
    KE_BUSY = -6,
} kstatus_t;

kstatus_t kestrel_task_create(const kestrel_task_cfg_t *cfg);
/* Boot-time configuration failure (before kestrel_start): record a crash record and halt. The
 * watchdog is not running yet, so the board stays halted for inspection instead of reset-looping.
 */
void kestrel_panic(uint32_t code) __attribute__((noreturn));
void kestrel_start(void) __attribute__((noreturn));
bool kestrel_dwt_ok(void);

/* ---- task-callable services (system calls) ---- */
void kestrel_wait_period(void); /* end of the current job; block until next release */
void kestrel_sleep_us(uint32_t us);
void kestrel_yield(void);
uint64_t kestrel_now(void);         /* 64-bit timebase ticks (48 MHz) */
void kestrel_health_vote(void);     /* critical tasks: "this job completed and checked" */
void kestrel_trace(uint16_t value); /* TE_USER event from a task */
void kestrel_assert_fail(uint32_t line) __attribute__((noreturn));
#define KESTREL_ASSERT(c)                                                                          \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            kestrel_assert_fail((uint32_t)__LINE__);                                               \
        }                                                                                          \
    } while (0)

/* Event flags: 32 flags; waiters block until any/all of mask are set, optional timeout. */
typedef struct {
    volatile uint32_t bits;
} kflags_t;
#define KFLAGS_ANY    0x0u
#define KFLAGS_ALL    0x1u
#define KFLAGS_CLEAR  0x2u /* clear the matched bits on successful wait */
#define KWAIT_FOREVER 0xFFFFFFFFu
/* Returns matched bits (nonzero) or 0 on timeout. */
uint32_t kestrel_flags_wait(kflags_t *f, uint32_t mask, uint32_t mode, uint32_t timeout_us);
void kestrel_flags_set(kflags_t *f, uint32_t bits);     /* tasks */
void kestrel_flags_set_isr(kflags_t *f, uint32_t bits); /* ISRs (privileged) */

/* Mutex. Protocol chosen per object:
 *   KMUTEX_PLAIN   : blocking, FIFO-by-priority handoff, NO inversion control (demonstrator only)
 *   KMUTEX_CEILING : immediate priority ceiling protocol; ceiling = highest user priority */
typedef struct {
    uint8_t protocol;
    uint8_t ceiling;        /* KMUTEX_CEILING: max priority of any task that locks it */
    volatile uint8_t owner; /* task id + 1; 0 = free */
    uint8_t reserved;
    uint32_t contentions;
} kmutex_t;
#define KMUTEX_PLAIN   0u
#define KMUTEX_CEILING 1u
kstatus_t kestrel_mutex_lock(kmutex_t *m);
kstatus_t kestrel_mutex_unlock(kmutex_t *m);

#endif
