/* Tiny latency histogram in timer ticks (20.83 ns), portable, shared by the ctxbench variants. */
#ifndef KESTREL_LATHIST_H
#define KESTREL_LATHIST_H
#include <stdint.h>

#define LATHIST_BINS  128u
#define LATHIST_SHIFT 4u /* bin width 16 ticks = 333 ns; range 0..42.7 us, overflow above */

typedef struct {
    uint32_t count;
    uint32_t max;
    uint32_t min;
    uint32_t overflow;
    uint32_t bins[LATHIST_BINS];
} lathist_t;

static inline void lathist_add(lathist_t *h, uint32_t v)
{
    if (h->count == 0u || v < h->min) {
        h->min = v;
    }
    h->count++;
    if (v > h->max) {
        h->max = v;
    }
    uint32_t b = v >> LATHIST_SHIFT;
    if (b < LATHIST_BINS) {
        h->bins[b]++;
    } else {
        h->overflow++;
    }
}
#endif
