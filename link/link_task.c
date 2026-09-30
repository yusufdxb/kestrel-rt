#include "link/link_task.h"

#include "arch/armv7m/cpu.h"
#include "drivers/uart/sci1_link.h"
#include "kernel/kernel_priv.h"
#include "kernel/kestrel.h"
#include "kernel/time/timebase.h"
#include "link/xlink.h"
#include "safety/crashlog/crashlog.h"
#include "trace/trace.h"

link_evidence_t g_link;
KESTREL_SHARED link_cfg_t g_link_cfg;

static xl_decoder_t s_dec;
static uint8_t s_carry[16]; /* time budget checked every 16 bytes */
static uint32_t s_carry_len, s_carry_off;
static bool s_muted_seen;
static xl_rx_t s_rx;
/* Double-buffered TX: frames are built into s_txb[s_build] (by RX handling and the periodic
 * emitter) while the DMA may still be sending the other buffer. */
static uint8_t s_txb[2][512];
static uint32_t s_build;
static uint32_t s_tx_len;
static uint32_t s_tx_seq;
static uint32_t s_telem_mask = 0xFFFFFFFFu;
static uint32_t s_metrics_ms = 1000u;
static uint32_t s_next_hb_ms, s_next_status_ms, s_next_metrics_ms;
static uint32_t s_ctl_jobs_last, s_ctl_miss_last;

/* Link-local flight recorder: link events in the kernel's 8-byte trace_event_t format, sent to the
 * ESP as TRACE chunks. Single producer and consumer (this task); the kernel trace ring, whose only
 * consumer is the SCI9 telemetry service, is not touched. Full ring: newest dropped and counted. */
#define LTR_SLOTS 32u
static trace_event_t s_ltr[LTR_SLOTS];
static uint32_t s_ltr_head, s_ltr_tail;

static void ltr(uint8_t type, uint8_t id, uint16_t data)
{
    if (s_ltr_head - s_ltr_tail >= LTR_SLOTS) {
        g_link.trace_dropped++;
        return;
    }
    trace_event_t *e = &s_ltr[s_ltr_head % LTR_SLOTS];
    e->timestamp = kt_now32();
    e->event = (uint16_t)(((uint16_t)type << 8) | id);
    e->data = data;
    s_ltr_head++;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(kt_now() / 48000u);
}

static void queue(uint8_t type, const uint8_t *p, uint16_t len)
{
    uint8_t *b = s_txb[s_build];
    if (s_tx_len + len + XL_OVERHEAD > sizeof(s_txb[0])) {
        g_link.tx_dropped++;
        return;
    }
    size_t n = xl_encode(&b[s_tx_len], sizeof(s_txb[0]) - s_tx_len, type, (uint16_t)g_link.epoch,
                         ++s_tx_seq, p, len);
    s_tx_len += (uint32_t)n;
    g_link.tx_frames++;
}

static void outage_begin(uint32_t t)
{
    ltr(LE_OUTAGE_BEGIN, 0u, 0u);
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
    uint32_t d = t - o->start_ms;
    ltr(LE_OUTAGE_END, 0u, (uint16_t)(d > 0xFFFFu ? 0xFFFFu : d));
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
        ltr(LE_SESSION, 0u, s_rx.session);
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
        ltr(LE_CONFIG, param, (uint16_t)ack[1]);
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
        uint8_t p[44];
        uint32_t jobs = g_k.tcb[0].jobs, miss = g_k.tcb[0].deadline_misses;
        uint32_t crit =
            g_k.tcb[0].deadline_misses + g_k.tcb[1].deadline_misses + g_k.tcb[2].deadline_misses;
        xl_put32(p, jobs - s_ctl_jobs_last);
        xl_put32(&p[4], miss - s_ctl_miss_last);
        xl_put32(&p[8], g_link.frames_ok);
        xl_put32(&p[12], g_link.dec_crc + g_link.dec_len + g_link.dec_version);
        xl_put32(&p[16], s_rx.rej_rate);
        xl_put32(&p[20], link_stats()->rx_overrun_bytes);
        xl_put32(&p[24], g_link.max_job_ticks);
        xl_put32(&p[28], g_link.loss_events);
        xl_put32(&p[32], crit);
        xl_put32(&p[36], g_k.tcb[0].max_start_lat); /* control start-latency max (ticks) */
        xl_put32(&p[40], g_link.rx_rejected);
        s_ctl_jobs_last = jobs;
        s_ctl_miss_last = miss;
        queue(XL_METRICS, p, sizeof(p));
        s_next_metrics_ms = t + s_metrics_ms;
    }
    if ((s_telem_mask & 4u) != 0u && s_ltr_head != s_ltr_tail) {
        uint8_t p[16u * sizeof(trace_event_t)];
        uint32_t n = 0u;
        while (s_ltr_tail != s_ltr_head && n < 16u) {
            const trace_event_t *e = &s_ltr[s_ltr_tail % LTR_SLOTS];
            xl_put32(&p[8u * n], e->timestamp);
            xl_put16(&p[8u * n + 4u], e->event);
            xl_put16(&p[8u * n + 6u], e->data);
            s_ltr_tail++;
            n++;
        }
        queue(XL_TRACE, p, (uint16_t)(8u * n));
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
    outage_begin(0u);        /* no session since boot: a logged outage like any other */
    g_link.loss_events = 0u; /* the boot interval is not a loss event */
}

void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t_start = kestrel_time32();
        uint32_t t = now_ms();
        uint32_t bytes = 0u, frames = 0u, rejected_this_job = 0u;
        if (link_service_errors(t) && !s_muted_seen) {
            s_muted_seen = true;
            ltr(LE_MUTE, 0u, (uint16_t)link_stats()->rx_storm_mutes);
        } else {
            s_muted_seen = false;
        }
#if LINK_N4_SYNTH_US
        { /* N4 control: synthetic communications load, no ESP traffic processed */
            uint32_t c0 = kcycles();
            while ((kcycles() - c0) < LINK_N4_SYNTH_US * 48u) {
            }
        }
#endif
        /* Bounded communications work: bytes, frames and time are all capped per job. Bytes read
         * from the ring but not decoded when the frame budget ends stay in s_carry for the next
         * job. */
        for (;;) {
            if (s_carry_off == s_carry_len) {
                if (bytes >= LINK_BUDGET_BYTES) {
                    g_link.budget_hit_bytes++;
                    ltr(LE_BUDGET, 1u, 0u);
                    break;
                }
                uint32_t want = LINK_BUDGET_BYTES - bytes;
                s_carry_len =
                    link_rx_read(s_carry, want < sizeof(s_carry) ? want : sizeof(s_carry));
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
#if LINK_N3_DISCARD
                g_link.n3_discarded++; /* N3 control: validated (CRC), then discarded */
                if (++frames >= LINK_BUDGET_FRAMES) {
                    frame_budget = true;
                    break;
                }
                continue;
#endif
                xl_verdict_t v = xl_rx_accept(&s_rx, &s_dec.f, t);
                if (v < 0) {
                    g_link.rx_rejected++;
                    rejected_this_job++;
                }
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
                ltr(LE_BUDGET, 2u, 0u);
                break;
            }
            if ((kestrel_time32() - t_start) > LINK_RX_BUDGET_TICKS) {
                g_link.budget_hit_time++;
                ltr(LE_BUDGET, 3u, 0u);
                break;
            }
        }
        if (rejected_this_job != 0u) {
            ltr(LE_REJECT, 0u,
                (uint16_t)(rejected_this_job > 0xFFFFu ? 0xFFFFu : rejected_this_job));
        }
        if (g_link.degraded == 0u && (t - g_link.last_valid_ms) > LINK_LOSS_MS) {
            outage_begin(g_link.last_valid_ms);
        }
        sync_counters();
        emit_periodic(t);
        if (s_tx_len != 0u) {
            if (!link_tx_busy() && link_tx_start(s_txb[s_build], s_tx_len) == 0) {
                s_build ^= 1u;
                s_tx_len = 0u;
            } else {
                g_link.tx_skipped_busy++; /* still draining: keep building, never wait */
            }
        }
        uint32_t dt = kestrel_time32() - t_start;
        if (dt > g_link.max_job_ticks) {
            g_link.max_job_ticks = dt;
        }
        kestrel_health_vote(); /* monitored only (noncritical): never gates the watchdog */
        kestrel_wait_period_ts(t_start);
    }
}
