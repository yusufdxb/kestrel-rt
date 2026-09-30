/* Phase 2 RA4 link service (priority 4, 5 ms period, privileged, noncritical).
 * Evidence that survives ESP loss is kept here, on the RA4, and read over the SCI9/USB path. */
#ifndef KESTREL_LINK_TASK_H
#define KESTREL_LINK_TASK_H
#include <stdint.h>

#define LINK_PERIOD_US 5000u
#define LINK_BUDGET_BYTES 512u   /* per job */
#define LINK_BUDGET_FRAMES 16u   /* per job */
#define LINK_BUDGET_TICKS (300u * 48u) /* 300 us of the 5 ms period (6 %), whole job */
#define LINK_RX_BUDGET_TICKS (200u * 48u) /* receive side; the rest is left for TX emission */
#define LINK_LOSS_MS 250u        /* no valid ESP frame for this long -> link degraded */
#define LINK_OUTAGE_LOG 16u
/* Negative-control build variants (docs/PHASE2_FAULT_MODEL.md), both off in the real image:
 *   LINK_N3_DISCARD=1   frames are decoded and CRC-validated, then discarded (no policy, no replies)
 *   LINK_N4_SYNTH_US=n  each link job burns n us of synthetic work (run with the ESP silent) */
#ifndef LINK_N3_DISCARD
#define LINK_N3_DISCARD 0
#endif
#ifndef LINK_N4_SYNTH_US
#define LINK_N4_SYNTH_US 0
#endif
/* Link-local trace event types (TRACE frames; id/data per type) */
enum {
    LE_SESSION = 1,      /* data: new session id */
    LE_OUTAGE_BEGIN = 2, /* ESP silent for LINK_LOSS_MS */
    LE_OUTAGE_END = 3,   /* data: outage length ms (saturating) */
    LE_MUTE = 4,         /* receiver muted by the error-storm guard; data: mute count */
    LE_BUDGET = 5,       /* id: 1 bytes, 2 frames, 3 time */
    LE_CONFIG = 6,       /* id: param; data: 1 applied, 0 rejected */
    LE_REJECT = 7,       /* data: frames rejected by the policy in this job */
};

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms; /* 0 while ongoing */
} link_outage_t;

typedef struct {
    uint32_t epoch;
    uint16_t session;
    uint16_t degraded;
    uint32_t sessions;
    uint32_t frames_ok;          /* accepted (incl. HELLO) */
    uint32_t dec_crc, dec_len, dec_version, dec_skipped; /* malformed at the codec */
    uint32_t rej_type, rej_len, rej_rate, rej_nosession, rej_session, rej_replay, rej_jump;
    uint32_t rej_epoch, rej_retired, rej_hello_seq;
    uint32_t pings, config_applied, config_rejected;
    uint32_t budget_hit_bytes, budget_hit_frames, budget_hit_time; /* jobs that stopped early */
    uint32_t max_job_ticks;
    uint32_t tx_frames, tx_skipped_busy, tx_dropped;
    uint32_t rx_rejected, trace_dropped, n3_discarded;
    uint32_t loss_events;
    uint32_t outage_total_ms;
    uint32_t last_valid_ms;
    link_outage_t outages[LINK_OUTAGE_LOG];
    uint32_t n_outages;
} link_evidence_t;

extern link_evidence_t g_link;

/* Pending noncritical configuration, applied by the owning task at its own safe point. */
typedef struct {
    volatile uint32_t seq;      /* incremented by the link task per accepted request */
    volatile uint8_t param;
    volatile uint32_t value;
} link_cfg_t;
extern link_cfg_t g_link_cfg;

void link_task(void *arg);
void link_service_init(void);
#endif
