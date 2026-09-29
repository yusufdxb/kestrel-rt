/* Kernel-private state and functions. Everything here lives in privileged SRAM; unprivileged
 * tasks cannot read or write it (MPU region 2). */
#ifndef KESTREL_KERNEL_PRIV_H
#define KESTREL_KERNEL_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/kestrel.h"

#define KNUM_TCB (KESTREL_MAX_TASKS + 1u) /* + idle */
#define KIDLE_ID KESTREL_MAX_TASKS
#define KNUM_LEVELS 32u                  /* level = 2*prio (+1 while holding a ceiling mutex) */
#define KNO_TIME UINT64_MAX

typedef enum {
    KS_UNUSED = 0,
    KS_READY,   /* includes the running task */
    KS_BLOCKED,
    KS_FAULTED,
} kstate_t;

typedef enum {
    KW_NONE = 0,
    KW_PERIOD,
    KW_SLEEP,
    KW_FLAGS,
    KW_MUTEX,
} kwait_t;

typedef struct ktcb {
    uint32_t *sp; /* saved PSP; MUST stay the first member (context_switch.S) */
    const kestrel_task_cfg_t *cfg;
    uint8_t id;
    uint8_t state;
    uint8_t base_level;
    uint8_t level;
    uint8_t wait;
    uint8_t wait_mode;
    uint8_t ceil_depth;
    uint8_t ceil_saved[4];
    bool privileged;
    bool critical;
    bool job_active;
    bool job_started;
    bool deadline_flagged;
    void *wait_obj;
    uint32_t *wait_frame;   /* caller's stacked r0..: result is written here at wakeup */
    uint32_t wait_mask;
    uint32_t wait_result;
    uint64_t wake_at;       /* sleep/timeout wakeup, KNO_TIME if none */
    uint64_t next_event;    /* earliest of release/deadline/wake; may be early, never late */
    uint64_t period;        /* ticks, 0 = aperiodic */
    uint64_t deadline;      /* relative, ticks */
    uint64_t next_release;  /* ideal time of the next release */
    uint64_t release_ideal; /* ideal release of the current job */
    uint64_t abs_deadline;
    uint64_t last_vote;
    uint64_t wd_window;
    uint32_t switched_in;   /* DWT cycles at last dispatch */
    uint32_t job_exec;      /* DWT cycles consumed by the current job so far */
    uint32_t start_lat;     /* ticks, current job */
    uint32_t release_lat;   /* ticks, current job */
    /* statistics (exact, on target) */
    uint32_t releases;
    uint32_t jobs;
    uint32_t deadline_misses;
    uint32_t overruns;
    uint32_t faults;
    uint32_t wcet;          /* max job execution, cycles */
    uint32_t max_response;  /* ticks */
    uint32_t max_release_lat;
    uint32_t max_start_lat;
    uint32_t min_start_lat;
    uint64_t run_cycles;    /* lifetime CPU time, cycles */
    uint32_t canary_hits;
} ktcb_t;

typedef struct {
    ktcb_t tcb[KNUM_TCB];
    ktcb_t *level_owner[KNUM_LEVELS];
    uint32_t ready_levels;  /* bit L set: owner of level L is READY */
    ktcb_t *current;
    uint32_t ntasks;
    bool started;
    uint64_t t0;            /* synchronous first release */
    uint64_t next_supervisor;
    uint64_t alarm_at;      /* currently programmed timer event */
    /* kernel counters */
    uint32_t context_switches;
    uint32_t pendsv_count;
    uint32_t alarm_count;
    uint32_t svc_count;
    uint32_t dwt_rearms;
    uint32_t max_crit_cycles;
    uint32_t max_alarm_cycles;  /* kernel timer ISR body */
    uint32_t max_sched_cycles;  /* PendSV C part (selection + MPU switch) */
    uint32_t last_sched_cycles;
    uint32_t degraded;          /* bits: noncritical tasks that missed their health window */
} kernel_t;

extern kernel_t g_k;

/* context_switch.S interface */
extern volatile uint32_t k_pendsv_entry;
extern volatile uint32_t k_pendsv_exit;
uint32_t *kestrel_sched_switch(uint32_t *old_sp);
void k_start_first(uint32_t *boot_psp_top) __attribute__((noreturn));

/* scheduler (callers hold kcrit) */
void k_make_ready(ktcb_t *t);
void k_block(ktcb_t *t, kwait_t why);
void k_wake(ktcb_t *t, uint32_t result); /* ready + deliver syscall result */
void k_reschedule(void);
void k_release_locked(ktcb_t *t, uint64_t now);
void k_job_end_locked(ktcb_t *t, uint64_t now);
void k_alarm_isr(void);
void k_recompute_alarm(uint64_t now);
void k_events_changed(ktcb_t *t); /* refresh t->next_event; pull the alarm in if earlier */
void k_set_level(ktcb_t *t, uint8_t level);
void k_task_fault(ktcb_t *t, uint16_t kind);

/* supervisor (kernel/watchdog) */
void k_supervisor_init(void);
void k_supervisor_tick(uint64_t now);
#define KSUPERVISOR_PERIOD_US 5000u

/* stack (safety/stack) */
#define KSTACK_FILL        0xA5A5A5A5u
#define KSTACK_CANARY      0x4B535443u /* "KSTC" */
#define KSTACK_GUARD_WORDS 8u           /* 32 B MPU guard block at the stack bottom */
#define KSTACK_CANARY_WORD KSTACK_GUARD_WORDS
void kstack_prepare(ktcb_t *t);
uint32_t kstack_high_water(const ktcb_t *t); /* bytes ever used */
bool kstack_canary_ok(const ktcb_t *t);

/* MPU (safety/mpu) */
void kmpu_init(void);
void kmpu_switch(const ktcb_t *t);
bool kmpu_enabled(void);
void kmpu_set_enabled(bool on); /* experiments only */

/* pointer validation for system calls from unprivileged tasks */
bool k_user_ptr_ok(const ktcb_t *caller, const void *p, uint32_t size);

/* DWT guard */
void k_dwt_guard(void);

/* syscalls implemented in C and dispatched by SVC_Handler */
void k_svc_dispatch(uint32_t *frame, uint32_t svc_num, uint32_t exc_return);

#endif
