#include "drivers/uart/sci1_link.h"

#include "arch/armv7m/cpu.h"
#include "drivers/gpio/gpio.h"
#include "drivers/icu/icu.h"

#define SCR_TIE  (1u << 7)
#define SCR_RIE  (1u << 6)
#define SCR_TE   (1u << 5)
#define SCR_RE   (1u << 4)
#define SCR_TEIE (1u << 2)
#define SSR_ORER (1u << 5)
#define SSR_FER  (1u << 4)
#define SSR_PER  (1u << 3)
#define SEMR_BGDM (1u << 6)
#define SEMR_ABCS (1u << 4)
#define SPTR_SPB2IO (1u << 2)
#define SPTR_SPB2DT (1u << 1)
#define PSEL_SCI1_3_5_7_9 0x05u
#define MSTPB30_SCI1 (1u << 30)
#define MSTPA22_DMAC (1u << 22)

static uint8_t s_ring[LINK_RX_RING] __attribute__((aligned(4)));
static uint32_t s_read_total; /* consumer's free-running byte count */
static uint32_t s_base_total; /* bytes accounted for by previous DMA arms */
static link_stats_t s_st;
static volatile bool s_tx_busy;
static volatile uint32_t s_tx_len;
static volatile uint32_t s_err_window;
static volatile bool s_muted;
static uint32_t s_mute_since_ms;
static bool s_mute_seen;

static void rx_arm(void)
{
    R_DMAC3->DMCNT = 0u;
    R_DMAC3->DMSAR = (uint32_t)&R_SCI1->RDR;
    R_DMAC3->DMDAR = (uint32_t)s_ring;
    R_DMAC3->DMTMD = (uint16_t)(1u << 14 | 0u << 12 | 0u << 8 | 1u); /* repeat, dst repeat area, byte */
    R_DMAC3->DMAMD = (uint16_t)(0u << 14 | 2u << 6);                 /* src fixed, dst increment */
    R_DMAC3->DMCRA = (LINK_RX_RING << 16) | LINK_RX_RING;
    R_DMAC3->DMCRB = 0u; /* 0 = 65536 repeats */
    R_DMAC3->DMINT = (uint8_t)(1u << 4);
    R_DMAC3->DMCNT = 1u;
}

/* Free-running count of bytes the DMA has written since init. */
static uint32_t rx_written_total(void)
{
    uint32_t b1, a, b2;
    do { /* DMCRB and DMCRAL are updated by the DMA: read until the block count is stable */
        b1 = R_DMAC3->DMCRB;
        a = R_DMAC3->DMCRA & 0xFFFFu;
        b2 = R_DMAC3->DMCRB;
    } while (b1 != b2);
    uint32_t blocks = (65536u - b1) & 0xFFFFu;
    uint32_t pos = (a == 0u) ? 0u : (LINK_RX_RING - a);
    return s_base_total + blocks * LINK_RX_RING + pos;
}

static void rx_dma_end_isr(void)
{
    R_DMAC3->DMSTS = 0u;
    s_base_total += 65536u * LINK_RX_RING;
    s_st.rx_rearms++;
    rx_arm();
}

static void eri_isr(void)
{
    uint8_t ssr = R_SCI1->SSR;
    s_st.rx_hw_errors++;
    (void)R_SCI1->RDR;
    R_SCI1->SSR = (uint8_t)(ssr & (uint8_t) ~(SSR_ORER | SSR_FER | SSR_PER));
    if (++s_err_window >= LINK_ERR_STORM && !s_muted) {
        R_SCI1->SCR = (uint8_t)(R_SCI1->SCR & (uint8_t) ~(SCR_RE | SCR_RIE));
        s_muted = true;
        s_st.rx_storm_mutes++;
    }
}

bool link_service_errors(uint32_t now_ms)
{
    if (s_muted) {
        if (!s_mute_seen) {
            s_mute_seen = true;
            s_mute_since_ms = now_ms;
        } else if ((now_ms - s_mute_since_ms) >= LINK_MUTE_MS) {
            s_mute_seen = false;
            s_err_window = 0u;
            s_muted = false;
            link_rx_enable(true);
            return false;
        }
        return true;
    }
    s_err_window = 0u;
    return false;
}

static void tx_dma_end_isr(void)
{
    R_DMAC2->DMSTS = 0u;
    R_SCI1->SCR = (uint8_t)((R_SCI1->SCR & (uint8_t)~SCR_TIE) | SCR_TEIE);
}

static void tei_isr(void)
{
    R_SCI1->SCR = (uint8_t)(R_SCI1->SCR & (uint8_t) ~(SCR_TIE | SCR_TEIE | SCR_TE));
    if (s_tx_busy) {
        s_st.tx_bytes += s_tx_len;
        s_tx_busy = false;
    }
}

int link_init(void)
{
    ra_prcr_unlock(KESTREL_PRCR_PRC1);
    R_MSTP->MSTPCRB &= ~MSTPB30_SCI1;
    R_SYSTEM->MSTPCRA &= ~MSTPA22_DMAC;
    ra_prcr_lock();
    (void)R_MSTP->MSTPCRB;

    R_SCI1->SCR = 0u;
    R_SCI1->SPTR = (uint8_t)(SPTR_SPB2IO | SPTR_SPB2DT);
    (void)gpio_set_peripheral(GPIO_PIN(5u, 1u), PSEL_SCI1_3_5_7_9); /* P501 TXD1 */
    (void)gpio_set_peripheral(GPIO_PIN(5u, 2u), PSEL_SCI1_3_5_7_9); /* P502 RXD1 */
    R_SCI1->SMR = 0u;
    R_SCI1->SCMR = 0xF2u;
    R_SCI1->SEMR = (uint8_t)(SEMR_BGDM | SEMR_ABCS);
    R_SCI1->BRR = (uint8_t)((KESTREL_PCLKA_HZ / (8u * LINK_BAUD)) - 1u); /* 5: exactly 1 Mbaud */
    uint32_t t0 = kcycles();
    while ((kcycles() - t0) < 200u) {
    }
    uint8_t ssr = R_SCI1->SSR;
    R_SCI1->SSR = (uint8_t)(ssr & (uint8_t) ~(SSR_ORER | SSR_FER | SSR_PER));

    R_DMA->DMAST = 1u;
    R_DMAC2->DMCNT = 0u;
    R_DMAC2->DMTMD = (uint16_t)(0u << 14 | 0u << 8 | 1u);
    R_DMAC2->DMAMD = (uint16_t)(2u << 14 | 0u << 6);
    R_DMAC2->DMDAR = (uint32_t)&R_SCI1->TDR;
    R_DMAC2->DMINT = (uint8_t)(1u << 4);
    R_ICU->DELSR[2] = EVT_SCI1_TXI;
    rx_arm();
    R_ICU->DELSR[3] = EVT_SCI1_RXI;

    icu_route_mode(IRQ_LINE_LINK_ERI, EVT_SCI1_ERI, eri_isr, KPRIO_DEVICE, ICU_LEVEL);
    icu_route_mode(IRQ_LINE_LINK_TEI, EVT_SCI1_TEI, tei_isr, KPRIO_DEVICE, ICU_LEVEL);
    icu_route_mode(IRQ_LINE_LINK_DMATX, EVT_DMAC2_INT, tx_dma_end_isr, KPRIO_DEVICE, ICU_LEVEL);
    icu_route_mode(IRQ_LINE_LINK_DMARX, EVT_DMAC3_INT, rx_dma_end_isr, KPRIO_DEVICE, ICU_LEVEL);
    s_read_total = 0u;
    s_base_total = 0u;
    R_SCI1->SCR = (uint8_t)(SCR_RE | SCR_RIE); /* RIE: RXI drives DMAC3 (not routed to the NVIC) */
    return 0;
}

uint32_t link_rx_pending(void)
{
    uint32_t avail = rx_written_total() - s_read_total;
    return avail > LINK_RX_RING ? LINK_RX_RING : avail;
}

uint32_t link_rx_read(uint8_t *buf, uint32_t max)
{
    uint32_t written = rx_written_total();
    uint32_t avail = written - s_read_total;
    if (avail > LINK_RX_RING - 16u) {
        /* The writer lapped (or is about to lap) the reader: skip to recent data, count the loss. */
        uint32_t skip = avail - (LINK_RX_RING / 2u);
        s_read_total += skip;
        s_st.rx_overrun_bytes += skip;
        s_st.rx_overrun_events++;
        avail -= skip;
    }
    uint32_t n = avail < max ? avail : max;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s_ring[(s_read_total + i) % LINK_RX_RING];
    }
    s_read_total += n;
    s_st.rx_bytes += n;
    return n;
}

bool link_tx_busy(void)
{
    return s_tx_busy;
}

int link_tx_start(const uint8_t *buf, uint32_t n)
{
    if (n == 0u || n > 1024u) {
        return -2;
    }
    kcrit_t s = kcrit_enter();
    if (s_tx_busy) {
        s_st.tx_refused_busy++;
        kcrit_exit(s);
        return -1;
    }
    s_tx_busy = true;
    s_tx_len = n;
    kcrit_exit(s);
    R_DMAC2->DMCNT = 0u;
    R_DMAC2->DMSTS = 0u;
    R_DMAC2->DMSAR = (uint32_t)buf;
    R_DMAC2->DMCRA = n;
    R_DMAC2->DMCNT = 1u;
    s_st.tx_chunks++;
    kcrit_t s2 = kcrit_enter();
    R_SCI1->SCR = (uint8_t)(R_SCI1->SCR | SCR_TE | SCR_TIE);
    kcrit_exit(s2);
    return 0;
}

const link_stats_t *link_stats(void)
{
    return &s_st;
}

void link_rx_enable(bool on)
{
    if (on) {
        rx_arm();
        s_read_total = rx_written_total();
        R_SCI1->SCR = (uint8_t)(R_SCI1->SCR | SCR_RE | SCR_RIE);
    } else {
        R_SCI1->SCR = (uint8_t)(R_SCI1->SCR & (uint8_t) ~(SCR_RE | SCR_RIE));
        R_DMAC3->DMCNT = 0u;
    }
}
