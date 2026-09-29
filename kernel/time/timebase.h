/* Monotonic 64-bit timebase: GPT320 (32-bit) free-running at PCLKD = 48 MHz, high word extended in
 * software by the overflow interrupt. One tick = 1/48 us = 20.833 ns. Rollover of the hardware
 * counter every 89.48 s; of the 64-bit count after ~12,000 years. */
#ifndef KESTREL_TIMEBASE_H
#define KESTREL_TIMEBASE_H
#include <stdbool.h>
#include <stdint.h>

#define KT_TICKS_PER_US 48u

typedef void (*kt_alarm_fn)(void);

void kt_init(kt_alarm_fn on_alarm);
/* 64-bit now; privileged callers only (bounded critical section, ~20 cycles). */
uint64_t kt_now(void);
/* Low 32 bits, readable without a critical section; fine for deltas < 89 s. */
static inline uint32_t kt_now32(void)
{
    return *(volatile uint32_t *)0x40078048u; /* GPT320.GTCNT */
}
static inline uint64_t kt_us_to_ticks(uint32_t us)
{
    return (uint64_t)us * KT_TICKS_PER_US;
}
/* Program the one-shot alarm (compare A) for absolute time `when`. If `when` is already in the
 * past or too close to program safely, the alarm interrupt is pended immediately. */
void kt_alarm_set(uint64_t when);

/* Test hooks for the rollover campaign (stop, preload the counter, restart). */
void kt_test_preload(uint32_t counter_low, uint32_t high);
uint32_t kt_overflow_count(void);

/* Pure helper (host-tested): extend a 32-bit counter sample with a software high word, given
 * whether an overflow was pending when the sample was taken. */
static inline uint64_t kt_extend(uint32_t high, uint32_t low, bool overflow_pending)
{
    if (overflow_pending && low < 0x80000000u) {
        high++;
    }
    return ((uint64_t)high << 32) | low;
}
#endif
