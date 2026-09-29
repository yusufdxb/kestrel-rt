/* Fixed-width histogram with an overflow bin (portable). Exact counts; percentiles are computed on
 * the host from the bins, so resolution = bin width. */
#ifndef KESTREL_KHIST_H
#define KESTREL_KHIST_H
#include <stdint.h>

#define KHIST_BINS 128u

typedef struct {
    uint8_t shift; /* bin width = 1 << shift */
    uint8_t id;
    uint16_t width;
    uint32_t count;
    uint32_t max;
    uint32_t overflow;
    uint32_t bins[KHIST_BINS];
} khist_t;

static inline void khist_add(khist_t *h, uint32_t v)
{
    h->count++;
    if (v > h->max) {
        h->max = v;
    }
    uint32_t b = v >> h->shift;
    if (b < KHIST_BINS) {
        h->bins[b]++;
    } else {
        h->overflow++;
    }
}

static inline void khist_reset(khist_t *h)
{
    h->count = 0u;
    h->max = 0u;
    h->overflow = 0u;
    for (uint32_t i = 0; i < KHIST_BINS; i++) {
        h->bins[i] = 0u;
    }
}
/* Kernel instrumentation histograms (kernel/stats.c): DWT cycles. */
extern khist_t g_hist_pendsv; /* PendSV entry -> exception return, width 4 */
extern khist_t g_hist_alarm;  /* kernel timer ISR body, width 8 */
extern khist_t g_hist_svc;    /* SVC dispatch body, width 4 */
extern khist_t g_hist_irqlat; /* timer compare match -> ISR, GPT ticks (20.8 ns), width 2 */
#endif
