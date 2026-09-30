/* Phase 2 interprocessor link: SCI1 (P501 TXD1 / P502 RXD1) through the board's TXB0108 level
 * translator to the ESP32-S3 UART1 (GPIO6 RX / GPIO5 TX). 1,000,000 baud 8N1 (exact on both sides:
 * 48 MHz / (8 * 6) on the RA4M1, 80 MHz / 80 on the ESP32-S3).
 *
 * TX: DMAC2 triggered by SCI1 TXI, same start/finish scheme as the SCI9 driver (TE|TIE in one
 *     write, completion at TEI). Nonblocking: a start while busy is refused and counted.
 * RX: DMAC3 in repeat mode writes into a 1 KB ring with no per-byte CPU work. The writer position
 *     is derived from the DMA counters: blocks done (DMCRB) * ring + (ring - DMCRAL), a free-running
 *     byte count, so a consumer that falls more than one ring behind is detected exactly and
 *     resynchronised (bytes counted as dropped) instead of reading overwritten data.
 * Error-storm guard: a peer can hold the line low (break) or send at the wrong rate and raise one
 *     receive-error interrupt per character time (~100 kHz, observed while the ESP32-S3 sat in ROM
 *     download mode). After LINK_ERR_STORM errors within one service window the receiver is muted
 *     (RE/RIE off, no more interrupts) and the service re-enables it after LINK_MUTE_MS. This bounds
 *     the interrupt load the untrusted side can put on the RA4M1.
 * This file is Phase 2 only; the Phase 1 SCI9 driver is unchanged. */
#ifndef KESTREL_SCI1_LINK_H
#define KESTREL_SCI1_LINK_H
#include <stdbool.h>
#include <stdint.h>

#define LINK_BAUD    1000000u
#define LINK_RX_RING 1024u
#define LINK_ERR_STORM 32u /* receive errors per service window before muting */
#define LINK_MUTE_MS   20u

typedef struct {
    uint32_t rx_bytes;         /* delivered to the consumer */
    uint32_t rx_overrun_bytes; /* lost because the consumer fell a full ring behind */
    uint32_t rx_overrun_events;
    uint32_t rx_hw_errors;     /* SSR ORER/FER/PER */
    uint32_t rx_rearms;        /* DMAC3 repeat count exhausted (every 64 MB) */
    uint32_t tx_bytes;
    uint32_t tx_refused_busy;
    uint32_t tx_chunks;
    uint32_t rx_storm_mutes;   /* receiver muted by the error-storm guard */
} link_stats_t;

int link_init(void);
/* Consumer side (single consumer, privileged task): copy up to max received bytes. */
uint32_t link_rx_read(uint8_t *buf, uint32_t max);
/* Bytes waiting in the ring (bounded by LINK_RX_RING). */
uint32_t link_rx_pending(void);
/* Start a DMA transmission of n bytes (1..1024); buffer must stay valid until link_tx_busy() is
 * false. Returns 0 on start, -1 if busy (refused, counted), -2 on bad length. */
int link_tx_start(const uint8_t *buf, uint32_t n);
bool link_tx_busy(void);
const link_stats_t *link_stats(void);
/* Once per service job: resets the error window, re-enables a muted receiver after LINK_MUTE_MS.
 * Returns true while muted. */
bool link_service_errors(uint32_t now_ms);
/* Test hook (negative control N1): true stops the RX DMA and the SCI1 receiver. */
void link_rx_enable(bool on);
#endif
