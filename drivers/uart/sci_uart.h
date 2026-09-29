/* SCI9 asynchronous UART (P109 TXD9 / P110 RXD9) to the ESP32-S3 USB bridge.
 * TX: DMAC channel 0 triggered by SCI9 TXI (nonblocking); polled TX exists only for the boot banner
 * and the fault path, where interrupts may be unusable. RX: RXI ISR -> SPSC byte queue. */
#ifndef KESTREL_SCI_UART_H
#define KESTREL_SCI_UART_H
#include "kernel/ipc/spsc.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UART_OK = 0,
    UART_EBUSY = -1,
    UART_EINVAL = -2,
} uart_status_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_overrun; /* SSR.ORER */
    uint32_t rx_framing; /* SSR.FER */
    uint32_t tx_bytes;
    uint32_t tx_dma_chunks;
    uint32_t tx_rejected_busy;
    uint32_t tei_spurious;
} uart_stats_t;

/* Called from the TEI ISR when a DMA transmission has fully left the shift register. */
typedef void (*uart_tx_done_fn)(uint32_t bytes_sent);

uart_status_t uart_init(uint32_t baud, uart_tx_done_fn on_tx_done);
/* Start a DMA transfer of n bytes (1..1024). The buffer must stay valid until on_tx_done. */
uart_status_t uart_tx_dma_start(const uint8_t *buf, uint32_t n);
bool uart_tx_busy(void);
/* Polled, blocking: boot banner and fault dump only. Never called from real-time paths. */
void uart_write_polled(const uint8_t *buf, uint32_t n);
/* Force the TX path idle (fault path takes over the UART). */
void uart_tx_abort(void);

kq_spsc_t *uart_rx_queue(void);
const uart_stats_t *uart_stats(void);

/* Fault-campaign hook: while stalled, uart_tx_dma_start refuses to start (simulates a TX path
 * that cannot keep up, independent of the host). */
void uart_set_tx_stall(bool stalled);
#endif
