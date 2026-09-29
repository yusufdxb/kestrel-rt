#include "drivers/adc/adc.h"

#include "arch/armv7m/cpu.h"
#include "drivers/icu/icu.h"
#include "kernel/ipc/spsc.h"
#include "kernel/kestrel.h"

#define MSTPD16_ADC0 (1u << 16)
#define MSTPD6_GPT16 (1u << 6)
#define ADCSR_ADST   (1u << 15)
#define ADCER_14BIT  (3u << 1)
#define ADC_CH       9u
#define EVT_GPT2_OVF 109u

#define ADC_QCAP 16u
KESTREL_SHARED static adc_sample_t s_q_storage[ADC_QCAP];
KESTREL_SHARED static kq_spsc_t s_q = {0, 0, ADC_QCAP - 1u, sizeof(adc_sample_t), (uint8_t *)s_q_storage, 0, 0};
KESTREL_SHARED static uint32_t s_last_seq;
KESTREL_SHARED static uint32_t s_gaps;

static uint32_t s_seq;
static uint32_t s_isr;

static void trigger_isr(void)
{
    R_ADC0->ADCSR |= ADCSR_ADST;
}

static void scan_end_isr(void)
{
    adc_sample_t smp = {(uint16_t)R_ADC0->ADDR[ADC_CH], 0u, ++s_seq, 0u};
    s_isr++;
    (void)kq_spsc_push(&s_q, &smp);
}

adc_status_t adc_init(void)
{
    ra_prcr_unlock(KESTREL_PRCR_PRC1);
    R_MSTP->MSTPCRD &= ~(MSTPD16_ADC0 | MSTPD6_GPT16);
    ra_prcr_lock();
    (void)R_MSTP->MSTPCRD;

    /* P014 as analog input: PMR = 0, ASEL = 1. */
    R_PMISC->PWPR = 0u;
    R_PMISC->PWPR = 0x40u;
    R_PFS->PORT[0].PIN[14].PmnPFS = (1u << 15);
    R_PMISC->PWPR = 0u;
    R_PMISC->PWPR = 0x80u;

    R_ADC0->ADCSR = 0u;             /* single scan, software start */
    R_ADC0->ADCER = ADCER_14BIT;
    R_ADC0->ADANSA[0] = (uint16_t)(1u << ADC_CH);
    R_ADC0->ADANSA[1] = 0u;

    /* GPT2: 48 MHz / 16 = 3 MHz, period 1500 counts => 2 kHz overflow. */
    R_GPT2->GTCR = (2u << 24);
    R_GPT2->GTUDDTYC = 1u;
    R_GPT2->GTPR = (3000000u / ADC_SAMPLE_HZ) - 1u;
    R_GPT2->GTCNT = 0u;
    icu_route(IRQ_LINE_ADC, EVT_ADC0_SCAN_END, scan_end_isr, KPRIO_DEVICE);
    icu_route(IRQ_LINE_ADC_TRIG, EVT_GPT2_OVF, trigger_isr, KPRIO_DEVICE);
    R_GPT2->GTCR |= 1u;
    return ADC_OK;
}

bool adc_take_latest(adc_sample_t *out)
{
    adc_sample_t s;
    bool any = false;
    while (kq_spsc_pop(&s_q, &s)) {
        if (s_last_seq != 0u && s.seq != s_last_seq + 1u) {
            s_gaps += s.seq - s_last_seq - 1u;
        }
        s_last_seq = s.seq;
        *out = s;
        any = true;
    }
    out->gaps = s_gaps;
    return any;
}

uint32_t adc_isr_count(void)
{
    return s_isr;
}
