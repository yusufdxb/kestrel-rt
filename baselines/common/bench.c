#include "baselines/common/bench.h"

#include "arch/armv7m/cpu.h"
#include "drivers/uart/sci_uart.h"
#include "kernel/ipc/spsc.h"
#include "kernel/khist.h"
#include "trace/frame.h"

#define TX_RING    2048u
#define JOB_RING   128u
#define KF_BSTATUS 0x09u

volatile uint16_t g_bench_units[BENCH_MAX_TASKS] = {20u, 10u, 10u, 0u, 50u, 20u, 0u, 0u};

khist_t g_hist_pendsv = {2u, 0u, 4u, 0u, 0u, 0u, {0u}};
khist_t g_hist_alarm = {3u, 1u, 8u, 0u, 0u, 0u, {0u}};
khist_t g_hist_svc = {2u, 2u, 4u, 0u, 0u, 0u, {0u}};
khist_t g_hist_irqlat = {1u, 3u, 2u, 0u, 0u, 0u, {0u}};

static job_record_t s_jobs_storage[JOB_RING];
static kq_spsc_t s_jobs = {0, 0, JOB_RING - 1u, sizeof(job_record_t), (uint8_t *)s_jobs_storage,
                           0, 0};
static uint8_t s_ring[TX_RING];
static _Atomic uint32_t s_head, s_tail;
static volatile uint32_t s_inflight;
static uint16_t s_seq;
static bench_stats_t s_st;
static uint32_t s_jobs_dropped;
static uint32_t s_in_ts[BENCH_MAX_TASKS];
static kframe_decoder_t s_dec;
static uint8_t s_frame[KFRAME_MAX_PAYLOAD + KFRAME_OVERHEAD];
static uint8_t s_pl[KFRAME_MAX_PAYLOAD];
static uint32_t s_next_status;
static const char *s_name;
static uint32_t s_uptime_hi, s_last_now;

static void kick_locked(void)
{
    if (s_inflight != 0u || uart_tx_busy()) {
        return;
    }
    uint32_t head = atomic_load(&s_head), tail = atomic_load(&s_tail);
    uint32_t used = head - tail;
    if (used == 0u) {
        return;
    }
    uint32_t off = tail & (TX_RING - 1u);
    uint32_t n = TX_RING - off;
    n = n > used ? used : n;
    n = n > 1024u ? 1024u : n;
    if (uart_tx_dma_start(&s_ring[off], n) == UART_OK) {
        s_inflight = n;
    }
}

static void on_tx_done(uint32_t n)
{
    atomic_store(&s_tail, atomic_load(&s_tail) + n);
    s_inflight = 0u;
    kcrit_t s = kcrit_enter();
    kick_locked();
    kcrit_exit(s);
}

static void send(uint8_t type, const uint8_t *p, uint16_t len)
{
    size_t n = kframe_encode(s_frame, sizeof(s_frame), type, s_seq, p, len);
    uint32_t head = atomic_load(&s_head);
    if (n == 0u || n > TX_RING - (head - atomic_load(&s_tail))) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_ring[(head + i) & (TX_RING - 1u)] = s_frame[i];
    }
    atomic_store(&s_head, head + (uint32_t)n);
    s_seq++;
}

void bench_init(const char *system_name)
{
    s_name = system_name;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    kframe_decoder_init(&s_dec);
    (void)uart_init(460800u, on_tx_done);
}

void bench_switched_in(uint32_t task)
{
    if (task < BENCH_MAX_TASKS) {
        s_in_ts[task] = kcycles();
    }
}

void bench_switched_out(uint32_t task)
{
    if (task < BENCH_MAX_TASKS) {
        s_st.run_cyc[task] += kcycles() - s_in_ts[task];
    }
}

uint32_t bench_task_cycles(uint32_t task)
{
    kcrit_t s = kcrit_enter();
    uint32_t v = (uint32_t)s_st.run_cyc[task] + (kcycles() - s_in_ts[task]);
    kcrit_exit(s);
    return v;
}

void bench_irq_latency(uint32_t ticks)
{
    khist_add(&g_hist_irqlat, ticks);
}

void bench_job(uint8_t task, uint32_t seq, uint32_t ideal, uint32_t t_start, uint32_t exec_cycles,
               uint32_t deadline_ticks)
{
    uint32_t now = bench_now32();
    uint32_t start_lat = t_start - ideal;
    uint32_t resp = now - ideal;
    uint8_t flags = JR_FLAG_TASK_OBSERVED;
    if (resp > deadline_ticks) {
        flags |= JR_FLAG_MISS;
    }
    if ((exec_cycles >> 4) > 0xFFFFu || (resp >> 4) > 0xFFFFu || start_lat > 0xFFFFu) {
        flags |= JR_FLAG_SATURATED;
    }
    job_record_t jr = {task,
                       flags,
                       (uint16_t)seq,
                       0u,
                       sat16(start_lat),
                       sat16(exec_cycles >> 4),
                       sat16(resp >> 4)};
    kcrit_t s = kcrit_enter();
    s_st.jobs[task]++;
    if ((flags & JR_FLAG_MISS) != 0u) {
        s_st.misses[task]++;
    }
    if (exec_cycles > s_st.wcet_cyc[task]) {
        s_st.wcet_cyc[task] = exec_cycles;
    }
    if (start_lat > s_st.max_start[task]) {
        s_st.max_start[task] = start_lat;
    }
    if (!kq_spsc_push(&s_jobs, &jr)) {
        s_jobs_dropped++;
    }
    kcrit_exit(s);
}

static void status(uint32_t now)
{
    uint8_t *p = s_pl;
    uint32_t o = 0u;
    if (now < s_last_now) {
        s_uptime_hi++;
    }
    s_last_now = now;
    kput32(&p[o], now);
    o += 4u;
    kput32(&p[o], s_uptime_hi);
    o += 4u;
    kput32(&p[o], s_jobs_dropped);
    o += 4u;
    kput32(&p[o], g_hist_irqlat.max);
    o += 4u;
    for (uint32_t i = 0; i < 6u; i++) {
        kput32(&p[o], s_st.jobs[i]);
        kput32(&p[o + 4u], s_st.misses[i]);
        kput32(&p[o + 8u], s_st.wcet_cyc[i]);
        kput32(&p[o + 12u], s_st.max_start[i]);
        kput32(&p[o + 16u], (uint32_t)s_st.run_cyc[i]);
        kput32(&p[o + 20u], (uint32_t)(s_st.run_cyc[i] >> 32));
        o += 24u;
    }
    for (uint32_t k = 0; s_name[k] != '\0' && k < 16u; k++) {
        p[o++] = (uint8_t)s_name[k];
    }
    send(KF_BSTATUS, p, (uint16_t)o);
}

static void hist(uint8_t which)
{
    const khist_t *h = &g_hist_irqlat;
    for (uint32_t part = 0; part < 4u; part++) {
        uint8_t *p = s_pl;
        uint32_t o = 0u;
        p[o++] = which;
        p[o++] = (uint8_t)part;
        kput16(&p[o], h->width);
        o += 2u;
        kput32(&p[o], h->count);
        kput32(&p[o + 4u], h->max);
        kput32(&p[o + 8u], h->overflow);
        o += 12u;
        for (uint32_t b = 0; b < 32u; b++) {
            kput32(&p[o], h->bins[part * 32u + b]);
            o += 4u;
        }
        send(KF_RESULT, p, (uint16_t)o);
    }
}

__attribute__((weak)) void bench_app_command(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    (void)cmd;
    (void)p;
    (void)len;
}

void bench_send(uint8_t type, const uint8_t *p, uint16_t len)
{
    send(type, p, len);
}

static void command(const kframe_decoder_t *d)
{
    if (d->type != KF_CMD || d->len == 0u) {
        return;
    }
    const uint8_t *a = &d->payload[1];
    uint8_t ack[9] = {d->payload[0], 0, 0, 0, 0, 0, 0, 0, 0};
    switch (d->payload[0]) {
    case 0x0A: { /* reset stats */
        kcrit_t s = kcrit_enter();
        for (uint32_t i = 0; i < BENCH_MAX_TASKS; i++) {
            s_st.wcet_cyc[i] = 0u;
            s_st.max_start[i] = 0u;
        }
        khist_reset(&g_hist_irqlat);
        kcrit_exit(s);
        break;
    }
    case 0x09: /* get_hist: only the ISR-latency histogram exists in baselines */
        if (d->len >= 2u && a[0] == 3u) {
            hist(3u);
        }
        return;
    case 0x10: /* set_work task, units */
        if (d->len >= 4u && a[0] < BENCH_MAX_TASKS) {
            g_bench_units[a[0]] = kget16(&a[1]);
        }
        break;
    case 0x04: /* set_trace: job mask only (0 pauses the job stream) */
        break;
    default:
        bench_app_command(d->payload[0], a, (uint16_t)(d->len - 1u));
        break;
    }
    send(KF_ACK, ack, sizeof(ack));
}

void bench_service(void)
{
    uint8_t b;
    for (uint32_t i = 0; i < 256u && kq_spsc_pop(uart_rx_queue(), &b); i++) {
        if (kframe_decode_byte(&s_dec, b) == KFD_FRAME) {
            command(&s_dec);
        }
    }
    uint32_t now = bench_now32();
    if ((int32_t)(now - s_next_status) >= 0) {
        status(now);
        s_next_status = now + 48000000u;
    }
    while (kq_spsc_count(&s_jobs) != 0u) {
        uint32_t n = kq_spsc_count(&s_jobs);
        n = n > 40u ? 40u : n;
        if (TX_RING - (atomic_load(&s_head) - atomic_load(&s_tail)) < n * 12u + KFRAME_OVERHEAD) {
            break;
        }
        for (uint32_t i = 0; i < n; i++) {
            (void)kq_spsc_pop(&s_jobs, &s_pl[i * 12u]);
        }
        send(KF_JOBS, s_pl, (uint16_t)(n * 12u));
    }
    kcrit_t s = kcrit_enter();
    kick_locked();
    kcrit_exit(s);
}
