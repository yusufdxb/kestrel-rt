/* Kestrel benchmark workload (docs/SPEC.md section 4):
 *
 *   task     prio period  deadline  privilege     role
 *   control  7    1 ms    1 ms      unprivileged  4x4 state update + PI law, critical
 *   safety   6    2 ms    2 ms      unprivileged  bounds checks, critical
 *   sensor   5    2 ms    2 ms      unprivileged  filter on sensor samples, critical
 *   comms    3    10 ms   10 ms     privileged    telemetry service (system task)
 *   health   2    100 ms  100 ms    unprivileged  freshness checks, noncritical
 *   logger   1    -       -         unprivileged  best-effort background work
 *
 * IPC: sensor -> control and control -> safety are latest-value mailboxes (control always wants
 * the newest sample; a stale value is detected by sequence number, never silently used). Work per
 * job is set by the host (app command 0x10) and applied at the start of the next job. */
#include "apps/common/workload.h"
#include "apps/fault_injection/inject.h"
#include "drivers/adc/adc.h"
#include "drivers/timer/gpt.h"
#include "kernel/ipc/mailbox.h"
#include "kernel/kernel_priv.h"
#include "kernel/kestrel.h"
#include "kernel/time/timebase.h"
#include "trace/frame.h"
#include "trace/telemetry.h"

enum { T_CONTROL = 0, T_SAFETY, T_SENSOR, T_COMMS, T_HEALTH, T_LOGGER, T_COUNT };

typedef struct {
    volatile uint16_t units[T_COUNT];
    volatile uint32_t violations;
    volatile uint32_t stale_sensor;
    volatile uint32_t stale_control;
    volatile uint32_t mb_busy;
    volatile uint32_t health_runs;
    volatile uint32_t logger_loops;
    volatile uint32_t adc_gaps;
} app_shared_t;

KESTREL_SHARED static app_shared_t g_app = {.units = {20u, 10u, 10u, 0u, 50u, 20u}};
KESTREL_SHARED static ctrl_state_t g_ctrl_state;
KESTREL_SHARED static sensor_msg_t g_sensor_state;
KESTREL_SHARED static sensor_msg_t mb_sensor_data;
KESTREL_SHARED static kmailbox_t mb_sensor = {0, sizeof(sensor_msg_t), (volatile uint8_t *)&mb_sensor_data, 0, 0};
KESTREL_SHARED static ctrl_state_t mb_ctrl_data;
KESTREL_SHARED static kmailbox_t mb_ctrl = {0, sizeof(ctrl_state_t), (volatile uint8_t *)&mb_ctrl_data, 0, 0};

KESTREL_STACK(stk_control, 1024);
KESTREL_STACK(stk_safety, 1024);
KESTREL_STACK(stk_sensor, 1024);
KESTREL_STACK(stk_comms, 2048);
KESTREL_STACK(stk_health, 1024);
KESTREL_STACK(stk_logger, 1024);

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        inject_check(T_CONTROL);
        sensor_msg_t m;
        uint32_t seq;
        kmb_status_t st = kmb_read(&mb_sensor, &m, &seq);
        if (st == KMB_OK) {
            work_control(&g_ctrl_state, &m, g_app.units[T_CONTROL]);
            kmb_write(&mb_ctrl, &g_ctrl_state);
            kestrel_health_vote();
        } else if (st == KMB_EMPTY) {
            kestrel_health_vote(); /* startup: no sample yet */
        } else {
            g_app.mb_busy++;
        }
        kestrel_wait_period();
    }
}

static void safety_task(void *arg)
{
    (void)arg;
    uint32_t last_seq = 0u;
    for (;;) {
        inject_check(T_SAFETY);
        ctrl_state_t c;
        sensor_msg_t m;
        uint32_t cs, ms;
        if (kmb_read(&mb_ctrl, &c, &cs) == KMB_OK && kmb_read(&mb_sensor, &m, &ms) == KMB_OK) {
            if (cs == last_seq) {
                g_app.stale_control++;
            }
            last_seq = cs;
            g_app.violations += work_safety(&c, &m, g_app.units[T_SAFETY]);
        }
        kestrel_health_vote();
        kestrel_wait_period();
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    uint32_t synthetic = 0u;
    for (;;) {
        inject_check(T_SENSOR);
        uint32_t sample;
        adc_sample_t s;
        if (adc_take_latest(&s)) {
            sample = s.code;
            g_app.adc_gaps = s.gaps;
        } else {
            sample = (synthetic += 37u) & 0x3FFFu;
        }
        work_sensor(&g_sensor_state, sample, g_app.units[T_SENSOR]);
        kmb_write(&mb_sensor, &g_sensor_state);
        kestrel_health_vote();
        kestrel_wait_period();
    }
}

static void comms_task(void *arg)
{
    (void)arg;
    for (;;) {
        telemetry_service();
        kestrel_health_vote(); /* monitored only: comms is noncritical */
        kestrel_wait_period();
    }
}

static void health_task(void *arg)
{
    (void)arg;
    uint32_t last_sensor = 0u;
    for (;;) {
        inject_check(T_HEALTH);
        sensor_msg_t m;
        uint32_t seq;
        if (kmb_read(&mb_sensor, &m, &seq) == KMB_OK) {
            if (seq == last_sensor) {
                g_app.stale_sensor++;
            }
            last_sensor = seq;
        }
        (void)work_background(g_app.units[T_HEALTH]);
        g_app.health_runs++;
        kestrel_health_vote();
        kestrel_wait_period();
    }
}

static void logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        inject_check(T_LOGGER);
        (void)work_background(g_app.units[T_LOGGER]);
        g_app.logger_loops++;
        kestrel_health_vote();
    }
}

static const kestrel_task_cfg_t k_tasks[T_COUNT] = {
    {"control", control_task, 0, 1000u, 1000u, 7u, KTASK_CRITICAL, stk_control, sizeof(stk_control), 10000u},
    {"safety", safety_task, 0, 2000u, 2000u, 6u, KTASK_CRITICAL, stk_safety, sizeof(stk_safety), 20000u},
    {"sensor", sensor_task, 0, 2000u, 2000u, 5u, KTASK_CRITICAL, stk_sensor, sizeof(stk_sensor), 20000u},
    {"comms", comms_task, 0, 10000u, 10000u, 3u, KTASK_PRIVILEGED, stk_comms, sizeof(stk_comms), 500000u},
    {"health", health_task, 0, 100000u, 100000u, 2u, 0u, stk_health, sizeof(stk_health), 1000000u},
    {"logger", logger_task, 0, 0u, 0u, 1u, 0u, stk_logger, sizeof(stk_logger), 2000000u},
};

/* App commands (comms task context). Applied by the tasks at their next job start. */
void kestrel_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    if (cmd == 0x10u && len >= 3u && p[0] < T_COUNT) { /* SET_WORK task, units */
        g_app.units[p[0]] = kget16(&p[1]);
    } else if (cmd == 0x20u && len >= 4u) {             /* INJECT kind, task, arg */
        g_inject.task = p[1];
        g_inject.arg = kget16(&p[2]);
        g_inject.kind = p[0];
    } else if (cmd == 0x22u && len >= 4u) {             /* STORM hz (0 = off) */
        storm_set_rate(kget32(p));
    } else if (cmd == 0x30u || cmd == 0x31u) {          /* DIAG timer compare / CPU speed */
        uint32_t d[4];
        if (cmd == 0x30u) {
            kt_diag_compare(d);
        } else {
            kt_diag_cpu(d);
        }
        uint8_t out[16];
        for (uint32_t i = 0; i < 4u; i++) {
            kput32(&out[4u * i], d[i]);
        }
        (void)telemetry_send(KF_RESULT, out, 16u);
    } else if (cmd == 0x32u) {                          /* DIAG timer ISR stage stamps */
        extern volatile uint32_t g_kprof[8];
        uint8_t out[32];
        for (uint32_t i = 0; i < 8u; i++) {
            kput32(&out[4u * i], g_kprof[i] - g_kprof[0]);
        }
        (void)telemetry_send(KF_RESULT, out, 32u);
    } else if (cmd == 0x23u && len >= 1u) {             /* MPU on/off (experiments) */
        kmpu_set_enabled(p[0] != 0u);
    } else if (cmd == 0x33u && len >= 1u) {             /* FCACHE on/off (experiments) */
        extern void kestrel_fcache(uint32_t on);
        kestrel_fcache(p[0]);
    } else if (cmd == 0x21u) {                          /* APP_STATS */
        uint8_t out[40];
        uint32_t o = 0u;
        const uint32_t v[] = {g_app.violations, g_app.stale_sensor, g_app.stale_control,
                              g_app.mb_busy, g_app.health_runs, g_app.logger_loops, g_app.adc_gaps,
                              mb_sensor.read_retries, mb_ctrl.read_retries, adc_isr_count()};
        for (uint32_t i = 0; i < 10u; i++) {
            kput32(&out[o], v[i]);
            o += 4u;
        }
        (void)telemetry_send(KF_RESULT, out, (uint16_t)o);
    }
}

void kestrel_main(void)
{
    (void)adc_init();
    for (uint32_t i = 0; i < T_COUNT; i++) {
        if (kestrel_task_create(&k_tasks[i]) != KOK) {
            KESTREL_ASSERT(0);
        }
    }
    kestrel_start();
}
