#include "drivers/uart/sci_uart.h"

#include "arch/armv7m/cpu.h"
#include "drivers/gpio/gpio.h"
#include "drivers/icu/icu.h"

#define SCR_TIE           (1u << 7)
#define SCR_RIE           (1u << 6)
#define SCR_TE            (1u << 5)
#define SCR_RE            (1u << 4)
#define SCR_TEIE          (1u << 2)
#define SSR_TDRE          (1u << 7)
#define SSR_RDRF          (1u << 6)
#define SSR_ORER          (1u << 5)
#define SSR_FER           (1u << 4)
#define SSR_PER           (1u << 3)
#define SSR_TEND          (1u << 2)
#define SEMR_BGDM         (1u << 6)
#define SEMR_ABCS         (1u << 4)
#define SPTR_SPB2IO       (1u << 2)
#define SPTR_SPB2DT       (1u << 1)
#define PSEL_SCI1_3_5_7_9 0x05u
#define MSTPB22_SCI9      (1u << 22)
#define MSTPA22_DMAC      (1u << 22)

#define UART_RX_CAPACITY 256u
static uint8_t s_rx_storage[UART_RX_CAPACITY];
static kq_spsc_t s_rx = {0, 0, UART_RX_CAPACITY - 1u, 1u, s_rx_storage, 0, 0};

static uart_stats_t s_stats;
static uart_tx_done_fn s_tx_done;
static volatile bool s_tx_busy;
static volatile uint32_t s_tx_len;
static volatile bool s_tx_stall;

static void rxi_isr(void)
{
    uint8_t b = R_SCI9->RDR;
    s_stats.rx_bytes++;
    (void)kq_spsc_push(&s_rx, &b);
}

static void eri_isr(void)
{
    uint8_t ssr = R_SCI9->SSR;
    if ((ssr & SSR_ORER) != 0u) {
        s_stats.rx_overrun++;
    }
    if ((ssr & SSR_FER) != 0u) {
        s_stats.rx_framing++;
    }
    (void)R_SCI9->RDR;
    /* Error flags clear by writing 0 after reading them as 1; keep TDRE/TEND untouched (write 1).
     */
    R_SCI9->SSR = (uint8_t)(ssr & (uint8_t) ~(SSR_ORER | SSR_FER | SSR_PER));
}

static void dma_end_isr(void)
{
    R_DMAC0->DMSTS = 0u; /* clear DTIF */
    /* Last byte is now in TDR/TSR; wait for transmit end before releasing the buffer. */
    R_SCI9->SCR = (uint8_t)((R_SCI9->SCR & (uint8_t)~SCR_TIE) | SCR_TEIE);
}

static void tei_isr(void)
{
    if (!s_tx_busy) {
        s_stats.tei_spurious++; /* defensive: never advance the ring twice for one chunk */
        R_SCI9->SCR = (uint8_t)(R_SCI9->SCR & (uint8_t) ~(SCR_TIE | SCR_TEIE | SCR_TE));
        return;
    }
    /* TE -> 0 with SPTR holding TXD high, so the next start can re-trigger TXI with TE|TIE. */
    R_SCI9->SCR = (uint8_t)(R_SCI9->SCR & (uint8_t) ~(SCR_TIE | SCR_TEIE | SCR_TE));
    uint32_t n = s_tx_len;
    s_stats.tx_bytes += n;
    s_tx_busy = false;
    if (s_tx_done != 0) {
        s_tx_done(n);
    }
}

uart_status_t uart_init(uint32_t baud, uart_tx_done_fn on_tx_done)
{
    /* ABCS = 1, BGDM = 1, CKS = 0: bit rate = PCLKA / (8 * (BRR + 1)). */
    uint32_t div = (KESTREL_PCLKA_HZ + 4u * baud) / (8u * baud);
    if (div < 1u || div > 256u) {
        return UART_EINVAL;
    }
    s_tx_done = on_tx_done;

    ra_prcr_unlock(KESTREL_PRCR_PRC1);
    R_MSTP->MSTPCRB &= ~MSTPB22_SCI9;
    R_SYSTEM->MSTPCRA &= ~MSTPA22_DMAC;
    ra_prcr_lock();
    (void)R_MSTP->MSTPCRB;

    R_SCI9->SCR = 0u;
    R_SCI9->SPTR = (uint8_t)(SPTR_SPB2IO | SPTR_SPB2DT); /* TXD idles high while TE = 0 */
    (void)gpio_set_peripheral(GPIO_PIN(1u, 9u), PSEL_SCI1_3_5_7_9);
    (void)gpio_set_peripheral(GPIO_PIN(1u, 10u), PSEL_SCI1_3_5_7_9);
    R_SCI9->SMR = 0u;     /* async, 8 data bits, no parity, 1 stop, PCLKA/1 */
    R_SCI9->SCMR = 0xF2u; /* reset value: LSB first, no invert, CHR1 = 1 (8-bit with CHR = 0) */
    R_SCI9->SEMR = (uint8_t)(SEMR_BGDM | SEMR_ABCS);
    R_SCI9->BRR = (uint8_t)(div - 1u);
    /* Wait at least one bit period before enabling (manual requirement after BRR write). */
    uint32_t t0 = kcycles();
    while ((kcycles() - t0) < (KESTREL_ICLK_HZ / baud) * 2u) {
    }
    uint8_t ssr = R_SCI9->SSR;
    R_SCI9->SSR = (uint8_t)(ssr & (uint8_t) ~(SSR_ORER | SSR_FER | SSR_PER));

    /* DMAC0: byte transfers from an incrementing source to the fixed TDR, one per TXI. */
    R_DMA->DMAST = 1u;
    R_DMAC0->DMCNT = 0u;
    R_DMAC0->DMTMD =
        (uint16_t)(0u << 14 | 0u << 8 | 1u);         /* normal mode, byte, peripheral trigger */
    R_DMAC0->DMAMD = (uint16_t)(2u << 14 | 0u << 6); /* src increment, dst fixed */
    R_DMAC0->DMDAR = (uint32_t)&R_SCI9->TDR;
    R_DMAC0->DMINT = (uint8_t)(1u << 4); /* DTIE */
    R_ICU->DELSR[0] = EVT_SCI9_TXI;

    icu_route(IRQ_LINE_UART_RXI, EVT_SCI9_RXI, rxi_isr, KPRIO_DEVICE);
    icu_route_mode(IRQ_LINE_UART_ERI, EVT_SCI9_ERI, eri_isr, KPRIO_DEVICE, ICU_LEVEL);
    icu_route_mode(IRQ_LINE_UART_TEI, EVT_SCI9_TEI, tei_isr, KPRIO_DEVICE, ICU_LEVEL);
    icu_route_mode(IRQ_LINE_DMA_TX, EVT_DMAC0_INT, dma_end_isr, KPRIO_DEVICE, ICU_LEVEL);

    R_SCI9->SCR = (uint8_t)(SCR_RE | SCR_RIE);
    return UART_OK;
}

bool uart_tx_busy(void)
{
    return s_tx_busy;
}

uart_status_t uart_tx_dma_start(const uint8_t *buf, uint32_t n)
{
    if (n == 0u || n > 1024u) {
        return UART_EINVAL;
    }
    kcrit_t s = kcrit_enter();
    if (s_tx_busy || s_tx_stall) {
        s_stats.tx_rejected_busy++;
        kcrit_exit(s);
        return UART_EBUSY;
    }
    s_tx_busy = true;
    s_tx_len = n;
    kcrit_exit(s);

    R_DMAC0->DMCNT = 0u;
    R_DMAC0->DMSTS = 0u;
    R_DMAC0->DMSAR = (uint32_t)buf;
    R_DMAC0->DMCRA = n;
    R_DMAC0->DMCNT = 1u;
    s_stats.tx_dma_chunks++;
    /* TE and TIE set by one write: the SCI raises TXI, which starts the DMA. */
    kcrit_t s2 = kcrit_enter();
    R_SCI9->SCR = (uint8_t)(R_SCI9->SCR | SCR_TE | SCR_TIE);
    kcrit_exit(s2);
    return UART_OK;
}

void uart_tx_abort(void)
{
    R_DMAC0->DMCNT = 0u;
    R_SCI9->SCR = (uint8_t)(R_SCI9->SCR & (uint8_t) ~(SCR_TIE | SCR_TEIE));
    s_tx_busy = false;
}

void uart_write_polled(const uint8_t *buf, uint32_t n)
{
    R_SCI9->SCR = (uint8_t)(R_SCI9->SCR | SCR_TE);
    for (uint32_t i = 0; i < n; i++) {
        while ((R_SCI9->SSR & SSR_TDRE) == 0u) {
        }
        R_SCI9->TDR = buf[i];
    }
    while ((R_SCI9->SSR & SSR_TEND) == 0u) {
    }
    /* Leave TE = 0 so a later DMA start can raise TXI by setting TE|TIE together. */
    R_SCI9->SCR = (uint8_t)(R_SCI9->SCR & (uint8_t)~SCR_TE);
}

kq_spsc_t *uart_rx_queue(void)
{
    return &s_rx;
}

const uart_stats_t *uart_stats(void)
{
    return &s_stats;
}

void uart_set_tx_stall(bool stalled)
{
    s_tx_stall = stalled;
}
