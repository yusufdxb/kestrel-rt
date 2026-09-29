/* ADC14 unit 0, channel AN009 (A0, P014), 14-bit, single scan started by a GPT2 (16-bit) overflow
 * interrupt at 2 kHz. The scan-end ISR pushes {code, seq} into an SPSC queue that lives in the
 * shared application region, so the unprivileged sensor task can consume it directly.
 * Queue full policy: REJECT (sample dropped, counted); the consumer sees the gap via seq. */
#ifndef KESTREL_ADC_H
#define KESTREL_ADC_H
#include <stdbool.h>
#include <stdint.h>

#define ADC_SAMPLE_HZ 2000u

typedef struct {
    uint16_t code;
    uint16_t reserved;
    uint32_t seq;
    uint32_t gaps; /* cumulative samples lost between consumer reads (filled by adc_take_latest) */
} adc_sample_t;

typedef enum { ADC_OK = 0, ADC_ETIMEOUT = -1 } adc_status_t;

adc_status_t adc_init(void);
/* Task context (unprivileged OK): drain the queue, return the newest sample. */
bool adc_take_latest(adc_sample_t *out);
uint32_t adc_isr_count(void);
#endif
