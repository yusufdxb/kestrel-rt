/* Phase 2 RA4 link service (priority 4, 5 ms period, privileged, noncritical).
 * Evidence that survives ESP loss is kept here, on the RA4, and read over the SCI9/USB path. */
#ifndef KESTREL_LINK_TASK_H
#define KESTREL_LINK_TASK_H
#include <stdint.h>

#define LINK_PERIOD_US 5000u
#define LINK_BUDGET_BYTES 512u   /* per job */
#define LINK_BUDGET_FRAMES 16u   /* per job */
#define LINK_BUDGET_TICKS (300u * 48u) /* 300 us of the 5 ms period (6 %) */
#define LINK_LOSS_MS 250u        /* no valid ESP frame for this long -> link degraded */
#define LINK_OUTAGE_LOG 16u

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
