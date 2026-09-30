/* Phase 2 image: the Phase 1 benchmark workload (unchanged task set and work) plus the ESP32-S3
 * link service at priority 4, below every critical task (docs/PHASE2_ARCHITECTURE.md).
 *
 *   task     prio period  deadline  privilege     role
 *   control  7    1 ms    1 ms      unprivileged  4x4 state update + PI law, critical
 *   safety   6    2 ms    2 ms      unprivileged  bounds checks, critical
 *   sensor   5    2 ms    2 ms      unprivileged  filter on sensor samples, critical
 *   link     4    5 ms    5 ms      privileged    ESP32-S3 mailbox, budgeted (Phase 2, noncritical)
 *   comms    3    10 ms   10 ms     privileged    telemetry service (system task)
 *   health   2    100 ms  100 ms    unprivileged  freshness checks, noncritical
 *   logger   1    -       -         unprivileged  best-effort background work
 *
 * IPC: sensor -> control and control -> safety are latest-value mailboxes (control always wants
 * the newest sample; a stale value is detected by sequence number, never silently used). Work per
 * job is set by the host (app command 0x10) and applied at the start of the next job.
 * Build with EXTRA_CFLAGS=-DKESTREL_PHASE2_LINK=0 for the B0 image (link service absent). */
#ifndef KESTREL_PHASE2_LINK
#define KESTREL_PHASE2_LINK 1
#endif
#include "apps/common/workload.h"
#include "apps/fault_injection/inject.h"
#include "drivers/adc/adc.h"
#include "drivers/timer/gpt.h"
#include "kernel/ipc/mailbox.h"
#include "kernel/kernel_priv.h"
#include "kernel/kestrel.h"
#include "kernel/time/timebase.h"
#include "drivers/uart/sci1_link.h"
#include "link/link_task.h"
#include "link/xlink.h"
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
KESTREL_SHARED static kmailbox_t mb_sensor = {0, sizeof(sensor_msg_t),
                                              (volatile uint8_t *)&mb_sensor_data, 0, 0};
KESTREL_SHARED static ctrl_state_t mb_ctrl_data;
KESTREL_SHARED static kmailbox_t mb_ctrl = {0, sizeof(ctrl_state_t),
                                            (volatile uint8_t *)&mb_ctrl_data, 0, 0};

KESTREL_STACK(stk_control, 1024);
KESTREL_STACK(stk_safety, 1024);
KESTREL_STACK(stk_sensor, 1024);
KESTREL_STACK(stk_comms, 2048);
KESTREL_STACK(stk_health, 1024);
KESTREL_STACK(stk_logger, 1024);
KESTREL_STACK(stk_link, 2048);

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        if (inject_check(T_CONTROL) == INJ_RETURN) {
            return;
        }
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
        kestrel_wait_period_ts(t_start);
    }
}

static void safety_task(void *arg)
{
    (void)arg;
    uint32_t last_seq = 0u;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        if (inject_check(T_SAFETY) == INJ_RETURN) {
            return;
        }
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
        kestrel_wait_period_ts(t_start);
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    uint32_t synthetic = 0u;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        if (inject_check(T_SENSOR) == INJ_RETURN) {
            return;
        }
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
        kestrel_wait_period_ts(t_start);
    }
}

static void comms_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        if (inject_check(T_COMMS) == INJ_RETURN) {
            return;
        }
        telemetry_service();
        kestrel_health_vote(); /* monitored only: comms is noncritical */
        kestrel_wait_period_ts(t_start);
    }
}

static void health_task(void *arg)
{
    (void)arg;
    uint32_t last_sensor = 0u;
    uint32_t cfg_seen = 0u;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        if (inject_check(T_HEALTH) == INJ_RETURN) {
            return;
        }
        if (g_link_cfg.seq != cfg_seen) { /* safe point: whitelisted noncritical settings only */
            cfg_seen = g_link_cfg.seq;
            uint32_t v = g_link_cfg.value;
            if (g_link_cfg.param == XL_PARAM_HEALTH_UNITS) {
                g_app.units[T_HEALTH] = (uint16_t)v;
            } else if (g_link_cfg.param == XL_PARAM_LOGGER_UNITS) {
                g_app.units[T_LOGGER] = (uint16_t)v;
            }
        }
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
        kestrel_wait_period_ts(t_start);
    }
}

static void logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (inject_check(T_LOGGER) == INJ_RETURN) {
            return;
        }
        (void)work_background(g_app.units[T_LOGGER]);
        g_app.logger_loops++;
        kestrel_health_vote();
    }
}

static const kestrel_task_cfg_t k_tasks[T_COUNT] = {
    {"control", control_task, 0, 1000u, 1000u, 7u, KTASK_CRITICAL, stk_control, sizeof(stk_control),
     10000u},
    {"safety", safety_task, 0, 2000u, 2000u, 6u, KTASK_CRITICAL, stk_safety, sizeof(stk_safety),
     20000u},
    {"sensor", sensor_task, 0, 2000u, 2000u, 5u, KTASK_CRITICAL, stk_sensor, sizeof(stk_sensor),
     20000u},
    {"comms", comms_task, 0, 10000u, 10000u, 3u, KTASK_PRIVILEGED, stk_comms, sizeof(stk_comms),
     500000u},
    {"health", health_task, 0, 100000u, 100000u, 2u, 0u, stk_health, sizeof(stk_health), 1000000u},
    {"logger", logger_task, 0, 0u, 0u, 1u, 0u, stk_logger, sizeof(stk_logger), 2000000u},
};
static const kestrel_task_cfg_t k_link_task = {
    "link", link_task, 0, LINK_PERIOD_US, LINK_PERIOD_US, 4u, KTASK_PRIVILEGED, stk_link,
    sizeof(stk_link), 500000u};

/* App commands (comms task context). Applied by the tasks at their next job start. */
void kestrel_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    if (cmd == 0x10u && len >= 3u && p[0] < T_COUNT) { /* SET_WORK task, units */
        g_app.units[p[0]] = kget16(&p[1]);
    } else if (cmd == 0x20u && len >= 4u) { /* INJECT kind, task, arg */
        g_inject.task = p[1];
        g_inject.arg = kget16(&p[2]);
        g_inject.kind = p[0];
    } else if (cmd == 0x22u && len >= 4u) { /* STORM hz (0 = off) */
        storm_set_rate(kget32(p));
    } else if (cmd == 0x30u || cmd == 0x31u) { /* DIAG timer compare / CPU speed */
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
#ifdef KESTREL_PROFILE
    } else if (cmd == 0x32u) { /* DIAG timer ISR stage stamps */
        extern volatile uint32_t g_kpath[6];
        extern volatile uint32_t g_kstage[8];
        uint8_t out[56];
        for (uint32_t i = 0; i < 6u; i++) {
            kput32(&out[4u * i], g_kpath[i]);
        }
        for (uint32_t i = 0; i < 8u; i++) {
            kput32(&out[24u + 4u * i], g_kstage[i]);
        }
        (void)telemetry_send(KF_RESULT, out, 56u);
        extern volatile uint32_t g_kentry_hist[40];
        uint8_t h[160];
        for (uint32_t i = 0; i < 40u; i++) {
            kput32(&h[4u * i], g_kentry_hist[i]);
        }
        (void)telemetry_send(KF_RESULT, h, 160u);
        extern volatile uint32_t g_crit_max, g_crit_max_pc, g_crit_over20us;
        uint8_t c[12];
        kput32(&c[0], g_crit_max);
        kput32(&c[4], g_crit_max_pc);
        kput32(&c[8], g_crit_over20us);
        (void)telemetry_send(KF_RESULT, c, 12u);
    } else if (cmd == 0x23u && len >= 1u) { /* MPU on/off (experiments) */
        kmpu_set_enabled(p[0] != 0u);
    } else if (cmd == 0x33u && len >= 1u) { /* FCACHE on/off (experiments) */
        extern void kestrel_fcache(uint32_t on);
        kestrel_fcache(p[0]);
#endif
    } else if (cmd == 0x40u) { /* LINK_STATS: evidence kept on the RA4 */
        const link_stats_t *ls = link_stats();
        uint8_t out[4u * 44u];
        const uint32_t v[] = {KESTREL_PHASE2_LINK, g_link.epoch, g_link.session, g_link.degraded,
                              g_link.sessions, g_link.frames_ok, g_link.dec_crc, g_link.dec_len,
                              g_link.dec_version, g_link.dec_skipped, g_link.rej_type,
                              g_link.rej_len, g_link.rej_rate, g_link.rej_nosession,
                              g_link.rej_session, g_link.rej_replay, g_link.rej_jump,
                              g_link.rej_epoch, g_link.rej_retired, g_link.rej_hello_seq,
                              g_link.pings, g_link.config_applied, g_link.config_rejected,
                              g_link.budget_hit_bytes, g_link.budget_hit_frames,
                              g_link.budget_hit_time, g_link.max_job_ticks, g_link.tx_frames,
                              g_link.tx_skipped_busy, g_link.loss_events, g_link.outage_total_ms,
                              g_link.last_valid_ms, g_link.n_outages, ls->rx_bytes,
                              ls->rx_overrun_bytes, ls->rx_overrun_events, ls->rx_hw_errors,
                              ls->rx_rearms, ls->tx_bytes, ls->tx_refused_busy, ls->tx_chunks,
                              (uint32_t)(kt_now() / 48000u), g_app.units[T_HEALTH],
                              g_app.units[T_LOGGER]};
        _Static_assert(sizeof(v) == sizeof(out), "LINK_STATS layout");
        for (uint32_t i = 0; i < 44u; i++) {
            kput32(&out[4u * i], v[i]);
        }
        (void)telemetry_send(KF_RESULT, out, (uint16_t)sizeof(out));
    } else if (cmd == 0x41u) { /* LINK_OUTAGES: last LINK_OUTAGE_LOG outage intervals */
        uint8_t out[4u + 8u * LINK_OUTAGE_LOG];
        kput32(out, g_link.n_outages);
        for (uint32_t i = 0; i < LINK_OUTAGE_LOG; i++) {
            kput32(&out[4u + 8u * i], g_link.outages[i].start_ms);
            kput32(&out[8u + 8u * i], g_link.outages[i].end_ms);
        }
        (void)telemetry_send(KF_RESULT, out, (uint16_t)sizeof(out));
    } else if (cmd == 0x21u) { /* APP_STATS */
        uint8_t out[40];
        uint32_t o = 0u;
        const uint32_t v[] = {g_app.violations, g_app.stale_sensor,     g_app.stale_control,
                              g_app.mb_busy,    g_app.health_runs,      g_app.logger_loops,
                              g_app.adc_gaps,   mb_sensor.read_retries, mb_ctrl.read_retries,
                              adc_isr_count()};
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
            kestrel_panic(0x7A5C0000u | i);
        }
    }
#if KESTREL_PHASE2_LINK
    link_service_init();
    if (kestrel_task_create(&k_link_task) != KOK) {
        kestrel_panic(0x7A5C00F0u);
    }
#endif
    kestrel_start();
}
