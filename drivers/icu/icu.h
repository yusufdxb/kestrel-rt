/* ICU event routing: RA4M1 peripherals raise events that the ICU links to one of 32 NVIC lines
 * through IELSRn. Kestrel assigns lines statically (below) and dispatches through one table. */
#ifndef KESTREL_ICU_H
#define KESTREL_ICU_H
#include <stdint.h>

/* Event numbers: RA4M1 User's Manual, ICU event table (same values as FSP bsp_elc.h). */
#define EVT_DMAC0_INT       17u
#define EVT_DMAC1_INT       18u
#define EVT_AGT0_INT        30u
#define EVT_WDT_UNDERFLOW   37u
#define EVT_ADC0_SCAN_END   41u
#define EVT_CAN0_ERROR      74u
#define EVT_CAN0_MAILBOX_RX 77u
#define EVT_CAN0_MAILBOX_TX 78u
#define EVT_GPT0_CMP_A      87u
#define EVT_GPT0_OVF        93u
#define EVT_GPT1_CMP_A      95u
#define EVT_GPT1_OVF        101u
#define EVT_SCI9_RXI        168u
#define EVT_SCI9_TXI        169u
#define EVT_SCI9_TEI        170u
#define EVT_SCI9_ERI        171u

/* Static NVIC line plan. */
enum kestrel_irq_line {
    IRQ_LINE_TIMER_CMP = 0, /* GPT0 compare A: releases, deadlines, timeouts, supervisor */
    IRQ_LINE_TIMER_OVF = 1, /* GPT0 overflow: 64-bit timebase extension */
    IRQ_LINE_UART_RXI = 2,  /* DMAC1 transfer end (RX ring re-arm); RXI itself drives DMAC1 */
    IRQ_LINE_UART_TEI = 3,
    IRQ_LINE_UART_ERI = 4,
    IRQ_LINE_DMA_TX = 5, /* DMAC0 transfer end (telemetry TX) */
    IRQ_LINE_ADC = 6,
    IRQ_LINE_STORM = 7, /* GPT1 overflow, programmable rate, fault campaign only */
    IRQ_LINE_CAN_RX = 8,
    IRQ_LINE_CAN_TX = 9,
    IRQ_LINE_CAN_ERR = 10,
    IRQ_LINE_ADC_TRIG = 11, /* GPT2 overflow: starts one ADC scan (2 kHz) */
    IRQ_LINE_COUNT
};

typedef void (*kestrel_isr_t)(void);

/* The ICU latches each event in IELSRn.IR. When to clear that latch depends on the source:
 *  ICU_EDGE : pulse events (GPT compare/overflow, ADC scan end, SCI RXI). Cleared BEFORE the
 *             handler so an event that recurs while the handler runs is kept, not lost.
 *  ICU_LEVEL: status-driven events (SCI TEI/ERI, DMAC transfer end). Cleared AFTER the handler
 *             has removed the cause; clearing first would re-latch at once and run it twice. */
#define ICU_EDGE  0u
#define ICU_LEVEL 1u

/* Link `event` to NVIC `line`, install `isr`, set priority, enable. Privileged, boot-time only. */
void icu_route_mode(uint32_t line, uint32_t event, kestrel_isr_t isr, uint32_t prio, uint32_t mode);
static inline void icu_route(uint32_t line, uint32_t event, kestrel_isr_t isr, uint32_t prio)
{
    icu_route_mode(line, event, isr, prio, ICU_EDGE);
}
void icu_disable(uint32_t line);
int icu_is_pending(uint32_t line);

/* Instrumentation: optional per-line entry hook used for ISR latency measurement. */
extern volatile uint32_t icu_last_entry_cycles[IRQ_LINE_COUNT];
#endif
