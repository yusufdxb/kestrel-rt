#include "link/link_task.h"

#include "arch/armv7m/cpu.h"
#include "drivers/uart/sci1_link.h"
#include "kernel/kernel_priv.h"
#include "kernel/kestrel.h"
#include "kernel/time/timebase.h"
#include "link/xlink.h"
#include "safety/crashlog/crashlog.h"

link_evidence_t g_link;
KESTREL_SHARED link_cfg_t g_link_cfg;

static xl_decoder_t s_dec;
static uint8_t s_carry[64];
static uint32_t s_carry_len, s_carry_off;
static xl_rx_t s_rx;
static uint8_t s_tx[1024];
static uint32_t s_tx_len;
static uint32_t s_tx_seq;
static uint32_t s_telem_mask = 0xFFFFFFFFu;
static uint32_t s_metrics_ms = 1000u;
static uint32_t s_next_hb_ms, s_next_status_ms, s_next_metrics_ms;
static uint32_t s_ctl_jobs_last, s_ctl_miss_last;

static uint32_t now_ms(void)
{
    return (uint32_t)(kt_now() / 48000u);
}

static void queue(uint8_t type, const uint8_t *p, uint16_t len)
{
    if (s_tx_len + len + XL_OVERHEAD > sizeof(s_tx)) {
        g_link.tx_skipped_busy++;
        return;
    }
    size_t n = xl_encode(&s_tx[s_tx_len], sizeof(s_tx) - s_tx_len, type, (uint16_t)g_link.epoch,
                         ++s_tx_seq, p, len);
    s_tx_len += (uint32_t)n;
    g_link.tx_frames++;
}

static void outage_begin(uint32_t t)
{
    g_link.degraded = 1u;
    g_link.loss_events++;
    link_outage_t *o = &g_link.outages[g_link.n_outages % LINK_OUTAGE_LOG];
    o->start_ms = t;
    o->end_ms = 0u;
    g_link.n_outages++;
}

static void outage_end(uint32_t t)
{
    if (g_link.degraded == 0u) {
        return;
    }
    g_link.degraded = 0u;
    link_outage_t *o = &g_link.outages[(g_link.n_outages - 1u) % LINK_OUTAGE_LOG];
    o->end_ms = t;
    g_link.outage_total_ms += t - o->start_ms;
}

static void send_fault_record(void)
{
    const kcrash_boot_info_t *ci = crashlog_info();
    uint8_t p[9 + sizeof(kcrash_record_t)];
    xl_put32(p, ci->boot_count);
    xl_put32(&p[4], ci->reset_cause);
    p[8] = ci->crash_present ? 1u : 0u;
    const uint8_t *r = (const uint8_t *)&ci->last;
    for (uint32_t i = 0; i < sizeof(kcrash_record_t); i++) {
        p[9 + i] = r[i];
    }
    queue(XL_FAULT, p, (uint16_t)sizeof(p));
}

static void handle(const xl_frame_t *f, xl_verdict_t v, uint32_t t)
{
    if (v == XLR_NEW_SESSION) {
        g_link.session = s_rx.session;
        g_link.sessions = s_rx.sessions;
        outage_end(t);
        send_fault_record();
        return;
    }
    switch (f->type) {
    case XL_PING: {
        g_link.pings++;
        uint8_t p[8];
        xl_put32(p, xl_get32(f->payload));
        xl_put32(&p[4], t);
        queue(XL_PONG, p, 8);
        break;
    }
    case XL_CONFIG_REQ: {
        uint8_t param = f->payload[0];
        uint32_t value = xl_get32(&f->payload[4]);
        uint8_t ack[6] = {param, 0, 0, 0, 0, 0};
        if (xl_config_valid(param, value)) {
            if (param == XL_PARAM_METRICS_MS) {
                s_metrics_ms = value; /* owned by this task: its own safe point is now */
            } else {
                g_link_cfg.param = param; /* applied by the owning task at its next loop start */
                g_link_cfg.value = value;
                g_link_cfg.seq++;
            }
            g_link.config_applied++;
            ack[1] = 1u;
        } else {
            g_link.config_rejected++;
        }
        xl_put32(&ack[2], value);
        queue(XL_CONFIG_ACK, ack, 6);
        break;
    }
    case XL_TELEM_CTRL:
        s_telem_mask = xl_get32(f->payload);
        break;
    case XL_TIME_SYNC:
    default:
        break; /* metadata only: never touches the RA4 timebase */
    }
}

static void emit_periodic(uint32_t t)
{
    if ((int32_t)(t - s_next_hb_ms) >= 0) {
        uint8_t p[23];
        xl_put32(p, g_link.epoch);
        xl_put32(&p[4], t);
        xl_put16(&p[8], g_link.session);
        xl_put32(&p[10], s_rx.last_seq);
        xl_put32(&p[14], g_k.tcb[0].jobs);
        xl_put32(&p[18], g_k.tcb[0].deadline_misses);
        p[22] = (uint8_t)g_link.degraded;
        queue(XL_HEARTBEAT, p, sizeof(p));
        s_next_hb_ms = t + 100u;
    }
    if ((s_telem_mask & 1u) != 0u && (int32_t)(t - s_next_status_ms) >= 0) {
        uint8_t p[40];
        uint32_t o = 0;
        for (uint32_t i = 0; i < 3u; i++) { /* critical tasks */
            xl_put32(&p[o], g_k.tcb[i].jobs);
            xl_put32(&p[o + 4], g_k.tcb[i].deadline_misses);
            xl_put32(&p[o + 8], g_k.tcb[i].max_start_lat);
            o += 12u;
        }
        xl_put32(&p[o], crashlog_info()->boot_count);
        o += 4u;
        queue(XL_STATUS, p, (uint16_t)o);
        s_next_status_ms = t + 500u;
    }
    if ((s_telem_mask & 2u) != 0u && (int32_t)(t - s_next_metrics_ms) >= 0) {
        uint8_t p[32];
        uint32_t jobs = g_k.tcb[0].jobs, miss = g_k.tcb[0].deadline_misses;
        xl_put32(p, jobs - s_ctl_jobs_last);
        xl_put32(&p[4], miss - s_ctl_miss_last);
        xl_put32(&p[8], g_link.frames_ok);
        xl_put32(&p[12], g_link.dec_crc + g_link.dec_len + g_link.dec_version);
        xl_put32(&p[16], s_rx.rej_rate);
        xl_put32(&p[20], link_stats()->rx_overrun_bytes);
        xl_put32(&p[24], g_link.max_job_ticks);
        xl_put32(&p[28], g_link.loss_events);
        s_ctl_jobs_last = jobs;
        s_ctl_miss_last = miss;
        queue(XL_METRICS, p, sizeof(p));
        s_next_metrics_ms = t + s_metrics_ms;
    }
}

static void sync_counters(void)
{
    g_link.dec_crc = s_dec.err_crc;
    g_link.dec_len = s_dec.err_len;
    g_link.dec_version = s_dec.err_version;
    g_link.dec_skipped = s_dec.bytes_skipped;
    g_link.frames_ok = s_rx.accepted;
    g_link.rej_type = s_rx.rej_type;
    g_link.rej_len = s_rx.rej_len;
    g_link.rej_rate = s_rx.rej_rate;
    g_link.rej_nosession = s_rx.rej_nosession;
    g_link.rej_session = s_rx.rej_session;
    g_link.rej_replay = s_rx.rej_replay;
    g_link.rej_jump = s_rx.rej_jump;
    g_link.rej_epoch = s_rx.rej_epoch;
    g_link.rej_retired = s_rx.rej_retired;
    g_link.rej_hello_seq = s_rx.rej_hello_seq;
}

void link_service_init(void)
{
    (void)link_init();
    xl_decoder_init(&s_dec);
    /* Epoch: unique per RA4 boot (VBATT boot counter) plus timing entropy. */
    g_link.epoch = (crashlog_info()->boot_count << 16) ^ (kcycles() & 0xFFFFu);
    if ((uint16_t)g_link.epoch == 0u) {
        g_link.epoch ^= 1u;
    }
    xl_rx_init(&s_rx, g_link.epoch, 0u);
    g_link.degraded = 1u; /* until the first session */
}

void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        uint32_t t = now_ms();
        uint32_t bytes = 0u, frames = 0u;
        (void)link_service_errors(t);
        /* Bounded communications work: bytes, frames and time are all capped per job. Bytes read
         * from the ring but not decoded when the frame budget ends stay in s_carry for the next job. */
        for (;;) {
            if (s_carry_off == s_carry_len) {
                if (bytes >= LINK_BUDGET_BYTES) {
                    g_link.budget_hit_bytes++;
                    break;
                }
                uint32_t want = LINK_BUDGET_BYTES - bytes;
                s_carry_len = link_rx_read(s_carry, want < sizeof(s_carry) ? want : sizeof(s_carry));
                s_carry_off = 0u;
                if (s_carry_len == 0u) {
                    break;
                }
                bytes += s_carry_len;
            }
            bool frame_budget = false;
            while (s_carry_off < s_carry_len) {
                if (xl_decode_byte(&s_dec, s_carry[s_carry_off++]) != XLD_FRAME) {
                    continue;
                }
                xl_verdict_t v = xl_rx_accept(&s_rx, &s_dec.f, t);
                if (v == XLR_ACCEPT || v == XLR_NEW_SESSION) {
                    g_link.last_valid_ms = t;
                    if (v == XLR_ACCEPT) {
                        outage_end(t);
                    }
                    handle(&s_dec.f, v, t);
                }
                if (++frames >= LINK_BUDGET_FRAMES) {
                    frame_budget = true;
                    break;
                }
            }
            if (frame_budget) {
                g_link.budget_hit_frames++;
                break;
            }
            if ((kestrel_time32() - t_start) > LINK_BUDGET_TICKS) {
                g_link.budget_hit_time++;
                break;
            }
        }
        if (g_link.degraded == 0u && (t - g_link.last_valid_ms) > LINK_LOSS_MS) {
            outage_begin(g_link.last_valid_ms);
        }
        sync_counters();
        if (!link_tx_busy()) {
            s_tx_len = 0u;
            emit_periodic(t);
            if (s_tx_len != 0u && link_tx_start(s_tx, s_tx_len) != 0) {
                g_link.tx_skipped_busy++;
            }
        } else {
            g_link.tx_skipped_busy++; /* previous transmission still draining: never wait */
        }
        uint32_t dt = kestrel_time32() - t_start;
        if (dt > g_link.max_job_ticks) {
            g_link.max_job_ticks = dt;
        }
        kestrel_health_vote(); /* monitored only (noncritical): never gates the watchdog */
        kestrel_wait_period_ts(t_start);
    }
}
