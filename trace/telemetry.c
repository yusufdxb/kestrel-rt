#include "trace/telemetry.h"

#include "arch/armv7m/cpu.h"
#include "drivers/uart/sci_uart.h"
#include "drivers/wdt/wdt.h"
#include "kernel/kernel_priv.h"
#include "kernel/khist.h"
#include "kernel/time/timebase.h"
#include "safety/crashlog/crashlog.h"
#include "safety/fault/fault.h"
#include "trace/trace.h"

#define TX_RING              2048u
#define STATUS_PERIOD_US     1000000u
#define MAX_JOBS_PER_FRAME   40u /* 40 * 12 = 480 B */
#define MAX_EVENTS_PER_FRAME 60u /* 60 * 8 = 480 B */
#define MAX_DMA_CHUNK        1024u

/* host commands */
enum {
    CMD_PING = 0x01,
    CMD_GET_CRASH = 0x02,
    CMD_CLEAR_CRASH = 0x03,
    CMD_SET_TRACE = 0x04,
    CMD_GET_TASKS = 0x05,
    CMD_TX_STALL = 0x06,
    CMD_TIME_SYNC = 0x07,
    CMD_RESET = 0x08,
    CMD_GET_HIST = 0x09,
    CMD_RESET_STATS = 0x0A,
    CMD_TRACE_ARM = 0x0B, /* u32 record mask, u32 trigger mask, u16 post-trigger events */
    CMD_TRACE_STREAM = 0x0C,
    CMD_APP_FIRST = 0x10,
};

static uint8_t s_ring[TX_RING];
static _Atomic uint32_t s_head; /* producer: comms task */
static _Atomic uint32_t s_tail; /* consumer: TEI ISR */
static volatile uint32_t s_inflight;
static uint16_t s_seq;
static uint64_t s_next_status;
static uint64_t s_stall_until;
static bool s_stalled;
static kframe_decoder_t s_dec;
static telemetry_stats_t s_stats;
static uint8_t s_frame[KFRAME_MAX_PAYLOAD + KFRAME_OVERHEAD];
static uint8_t s_payload[KFRAME_MAX_PAYLOAD];

uint32_t kestrel_idle_loops(void);
uint32_t k_supervisor_warnings(void);

__attribute__((weak)) void kestrel_app_command(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    (void)cmd;
    (void)payload;
    (void)len;
}

static void kick_locked(void)
{
    if (s_inflight != 0u || uart_tx_busy()) {
        return;
    }
    uint32_t head = atomic_load_explicit(&s_head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
    uint32_t used = head - tail;
    if (used == 0u) {
        return;
    }
    uint32_t off = tail & (TX_RING - 1u);
    uint32_t n = TX_RING - off;
    if (n > used) {
        n = used;
    }
    if (n > MAX_DMA_CHUNK) {
        n = MAX_DMA_CHUNK;
    }
    if (uart_tx_dma_start(&s_ring[off], n) == UART_OK) {
        s_inflight = n;
    }
}

static void on_tx_done(uint32_t n)
{
    /* TEI ISR context (device priority). Bounded: one pointer update + one DMA start. */
    atomic_store_explicit(&s_tail, atomic_load_explicit(&s_tail, memory_order_relaxed) + n,
                          memory_order_release);
    s_inflight = 0u;
    kcrit_t s = kcrit_enter();
    kick_locked();
    kcrit_exit(s);
}

static uint32_t ring_free(void)
{
    return TX_RING - (atomic_load_explicit(&s_head, memory_order_relaxed) -
                      atomic_load_explicit(&s_tail, memory_order_acquire));
}

uint32_t telemetry_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    size_t n = kframe_encode(s_frame, sizeof(s_frame), type, s_seq, payload, len);
    if (n == 0u || n > ring_free()) {
        s_stats.ring_full++;
        return 0u;
    }
    uint32_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        s_ring[(head + i) & (TX_RING - 1u)] = s_frame[i];
    }
    atomic_store_explicit(&s_head, head + (uint32_t)n, memory_order_release);
    s_seq++;
    s_stats.frames_sent++;
    s_stats.bytes_queued += (uint32_t)n;
    return (uint32_t)n;
}

static void drain_ring(kq_spsc_t *q, uint8_t type, uint32_t rec_size, uint32_t max_per_frame)
{
    while (kq_spsc_count(q) != 0u) {
        uint32_t avail = kq_spsc_count(q);
        uint32_t n = avail < max_per_frame ? avail : max_per_frame;
        if (ring_free() < n * rec_size + KFRAME_OVERHEAD) {
            s_stats.ring_full++;
            return; /* leave records in the kernel ring; it counts its own drops */
        }
        for (uint32_t i = 0; i < n; i++) {
            (void)kq_spsc_pop(q, &s_payload[i * rec_size]);
        }
        (void)telemetry_send(type, s_payload, (uint16_t)(n * rec_size));
    }
}

static uint32_t put_u32(uint8_t *p, uint32_t off, uint32_t v)
{
    kput32(&p[off], v);
    return off + 4u;
}

static void send_status(uint64_t now)
{
    const trace_stats_t *ts = trace_stats();
    const uart_stats_t *us = uart_stats();
    const kcrash_boot_info_t *ci = crashlog_info();
    uint8_t *p = s_payload;
    uint32_t o = 0u;
    /* Stack scans walk up to 2 KB each: done outside the critical section (values are monotonic
     * high-water marks, so a slightly stale read is still a valid lower bound). */
    uint16_t hw[KNUM_TCB];
    for (uint32_t i = 0; i < KNUM_TCB; i++) {
        hw[i] = (g_k.tcb[i].cfg != 0) ? (uint16_t)kstack_high_water(&g_k.tcb[i]) : 0u;
    }
    /* Counters are copied field by field with interrupts enabled; each 32-bit read is atomic and
     * the status frame is a monitoring snapshot, not a consistent cut. */
    o = put_u32(p, o, (uint32_t)now);
    o = put_u32(p, o, (uint32_t)(now >> 32));
    o = put_u32(p, o, ci->boot_count);
    o = put_u32(p, o, ci->reset_cause);
    o = put_u32(p, o, g_k.context_switches);
    o = put_u32(p, o, g_k.alarm_count);
    o = put_u32(p, o, g_k.svc_count);
    o = put_u32(p, o, g_k.max_alarm_cycles);
    o = put_u32(p, o, g_k.max_sched_cycles);
    o = put_u32(p, o, g_k.max_crit_cycles);
    o = put_u32(p, o, g_hist_pendsv.max);
    o = put_u32(p, o, ts->events);
    o = put_u32(p, o, ts->events_dropped);
    o = put_u32(p, o, ts->jobs);
    o = put_u32(p, o, ts->jobs_dropped);
    o = put_u32(p, o, us->rx_bytes);
    o = put_u32(p, o, us->rx_overrun + us->rx_framing);
    o = put_u32(p, o, us->tx_bytes);
    o = put_u32(p, o, 0u); /* RX is a DMA ring: no queue rejects (field kept for format) */
    o = put_u32(p, o, s_dec.frames_ok);
    o = put_u32(p, o, s_dec.err_crc + s_dec.err_len + s_dec.err_version);
    o = put_u32(p, o, g_k.dwt_rearms);
    o = put_u32(p, o, g_k.degraded);
    o = put_u32(p, o, k_supervisor_warnings());
    o = put_u32(p, o, wdt_feeds());
    o = put_u32(p, o, kfault_count());
    o = put_u32(p, o, s_stats.ring_full);
    o = put_u32(p, o, g_k.ntasks + 1u);
    for (uint32_t i = 0; i < KNUM_TCB; i++) {
        const ktcb_t *t = &g_k.tcb[i];
        if (t->cfg == 0) {
            continue;
        }
        p[o++] = t->id;
        p[o++] = t->state;
        kput16(&p[o], hw[i]);
        o += 2u;
        o = put_u32(p, o, t->jobs);
        o = put_u32(p, o, t->deadline_misses);
        o = put_u32(p, o, t->overruns);
        o = put_u32(p, o, t->wcet);
        o = put_u32(p, o, t->max_response);
        o = put_u32(p, o, t->max_start_lat);
        o = put_u32(p, o, t->faults);
        kcrit_t s = kcrit_enter(); /* 64-bit counter: read both halves consistently */
        uint64_t rc = t->run_cycles;
        kcrit_exit(s);
        o = put_u32(p, o, (uint32_t)rc);
        o = put_u32(p, o, (uint32_t)(rc >> 32));
    }
    (void)telemetry_send(KF_STATUS, p, (uint16_t)o);
}

static void send_crash(void)
{
    const kcrash_boot_info_t *ci = crashlog_info();
    uint8_t *p = s_payload;
    uint32_t o = 0u;
    o = put_u32(p, o, ci->boot_count);
    o = put_u32(p, o, ci->reset_cause);
    p[o++] = ci->crash_present ? 1u : 0u;
    const uint8_t *r = (const uint8_t *)&ci->last;
    for (uint32_t i = 0; i < sizeof(kcrash_record_t); i++) {
        p[o++] = r[i];
    }
    (void)telemetry_send(KF_CRASH, p, (uint16_t)o);
}

static void send_tasks(void)
{
    uint8_t *p = s_payload;
    uint32_t o = 0u;
    for (uint32_t i = 0; i < KNUM_TCB; i++) {
        const ktcb_t *t = &g_k.tcb[i];
        if (t->cfg == 0) {
            continue;
        }
        p[o++] = t->id;
        p[o++] = (uint8_t)(t->base_level / 2u);
        p[o++] = t->cfg->flags;
        p[o++] = 0u;
        o = put_u32(p, o, t->cfg->period_us);
        o = put_u32(p, o, t->cfg->deadline_us);
        o = put_u32(p, o, t->cfg->stack_size);
        const char *nm = t->cfg->name;
        bool done = (nm == 0);
        for (uint32_t k = 0; k < 12u; k++) {
            char c = done ? '\0' : nm[k];
            done = done || (c == '\0');
            p[o++] = (uint8_t)c;
        }
    }
    (void)telemetry_send(KF_TASKS, p, (uint16_t)o);
}

static void send_hist(uint8_t which)
{
    const khist_t *h = which == 0u   ? &g_hist_pendsv
                       : which == 1u ? &g_hist_alarm
                       : which == 2u ? &g_hist_svc
                                     : &g_hist_irqlat;
    /* bins are sent in 4 frames of 32 bins to stay under the payload limit */
    for (uint32_t part = 0; part < 4u; part++) {
        uint8_t *p = s_payload;
        uint32_t o = 0u;
        p[o++] = which;
        p[o++] = (uint8_t)part;
        kput16(&p[o], h->width);
        o += 2u;
        o = put_u32(p, o, h->count);
        o = put_u32(p, o, h->max);
        o = put_u32(p, o, h->overflow);
        for (uint32_t b = 0; b < 32u; b++) {
            o = put_u32(p, o, h->bins[part * 32u + b]);
        }
        (void)telemetry_send(KF_RESULT, p, (uint16_t)o);
    }
}

static void ack(uint8_t cmd, uint32_t a, uint32_t b)
{
    uint8_t p[9];
    p[0] = cmd;
    kput32(&p[1], a);
    kput32(&p[5], b);
    (void)telemetry_send(KF_ACK, p, sizeof(p));
}

static void handle_command(const kframe_decoder_t *d, uint64_t now)
{
    if (d->type != KF_CMD || d->len == 0u) {
        s_stats.cmds_bad++;
        return;
    }
    uint8_t cmd = d->payload[0];
    const uint8_t *arg = &d->payload[1];
    uint16_t alen = (uint16_t)(d->len - 1u);
    s_stats.cmds_ok++;
    switch (cmd) {
    case CMD_PING:
        ack(cmd, alen >= 4u ? kget32(arg) : 0u, 0u);
        break;
    case CMD_GET_CRASH:
        send_crash();
        break;
    case CMD_CLEAR_CRASH:
        crashlog_clear();
        ack(cmd, 0u, 0u);
        break;
    case CMD_SET_TRACE:
        if (alen >= 5u) {
            trace_set_mask(kget32(arg), arg[4]);
        }
        ack(cmd, 0u, 0u);
        break;
    case CMD_GET_TASKS:
        send_tasks();
        break;
    case CMD_TX_STALL:
        if (alen >= 4u) {
            s_stall_until = now + kt_us_to_ticks(kget32(arg) * 1000u);
            s_stalled = true;
            uart_set_tx_stall(true);
        }
        break;
    case CMD_TIME_SYNC:
        ack(cmd, (uint32_t)now, (uint32_t)(now >> 32));
        break;
    case CMD_RESET:
        NVIC_SystemReset();
        break;
    case CMD_TRACE_ARM:
        if (alen >= 10u) {
            trace_arm(kget32(arg), kget32(&arg[4]), kget16(&arg[8]));
        }
        ack(cmd, 0u, 0u);
        break;
    case CMD_TRACE_STREAM:
        trace_stream();
        ack(cmd, 0u, 0u);
        break;
    case CMD_GET_HIST:
        send_hist(alen >= 1u ? arg[0] : 0u);
        break;
    case CMD_RESET_STATS: {
        kcrit_t s = kcrit_enter();
        khist_reset(&g_hist_pendsv);
        khist_reset(&g_hist_alarm);
        khist_reset(&g_hist_svc);
        khist_reset(&g_hist_irqlat);
        for (uint32_t i = 0; i < KNUM_TCB; i++) {
            ktcb_t *t = &g_k.tcb[i];
            t->wcet = 0u;
            t->max_response = 0u;
            t->max_release_lat = 0u;
            t->max_start_lat = 0u;
            t->min_start_lat = 0xFFFFFFFFu;
        }
        g_k.max_alarm_cycles = 0u;
        g_k.max_sched_cycles = 0u;
        g_k.max_crit_cycles = 0u;
        kcrit_exit(s);
        ack(cmd, 0u, 0u);
        break;
    }
    default:
        if (cmd >= CMD_APP_FIRST) {
            kestrel_app_command(cmd, arg, alen);
            ack(cmd, 0u, 0u);
        } else {
            s_stats.cmds_bad++;
        }
        break;
    }
}

void telemetry_init(void)
{
    kframe_decoder_init(&s_dec);
    (void)uart_init(460800u, on_tx_done);
}

void telemetry_service(void)
{
    uint64_t now = kt_now();
    /* 1. commands: bounded by the RX queue capacity (256 bytes per pass) */
    uint8_t rx[128];
    uint32_t nrx = uart_rx_read(rx, sizeof(rx));
    for (uint32_t i = 0; i < nrx; i++) {
        uint8_t b = rx[i];
        if (kframe_decode_byte(&s_dec, b) == KFD_FRAME) {
            handle_command(&s_dec, now);
        }
    }
    if (s_stalled && now >= s_stall_until) {
        s_stalled = false;
        uart_set_tx_stall(false);
    }
    s_stats.stall_until_lo = s_stalled ? (uint32_t)s_stall_until : 0u;
    /* 2. deferred crash-record persistence (preemptible, low priority) */
    (void)crashlog_persist();
    /* 3. status once per second */
    if (now >= s_next_status) {
        send_status(now);
        s_next_status = now + kt_us_to_ticks(STATUS_PERIOD_US);
    }
    /* 3. records */
    drain_ring(trace_job_ring(), KF_JOBS, sizeof(job_record_t), MAX_JOBS_PER_FRAME);
    if (trace_mode() != TRACE_ARMED) {
        drain_ring(trace_event_ring(), KF_TRACE, sizeof(trace_event_t), MAX_EVENTS_PER_FRAME);
    }
    /* 4. transmit */
    kcrit_t s = kcrit_enter();
    kick_locked();
    kcrit_exit(s);
}

const telemetry_stats_t *telemetry_stats(void)
{
    return &s_stats;
}
