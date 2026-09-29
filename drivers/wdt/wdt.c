#include "drivers/wdt/wdt.h"

#include "arch/armv7m/cpu.h"

#define WDTCR_RPSS_100 (3u << 12)
#define WDTCR_RPES_0   (3u << 8)
#define WDTCR_CKS_8192 (8u << 4)
#define WDTCR_TOPS_1024 (0u << 0)
#define WDTRCR_RESET   0x80u

static bool s_running;
static uint32_t s_feeds;

wdt_status_t wdt_start(void)
{
    if (s_running) {
        return WDT_EALREADY;
    }
    R_WDT->WDTCR = (uint16_t)(WDTCR_RPSS_100 | WDTCR_RPES_0 | WDTCR_CKS_8192 | WDTCR_TOPS_1024);
    R_WDT->WDTRCR = WDTRCR_RESET;
    R_WDT->WDTCSTPR = 0x80u;
    s_running = true;
    wdt_feed(); /* first refresh starts the counter in register-start mode */
    return WDT_OK;
}

void wdt_feed(void)
{
    R_WDT->WDTRR = 0x00u;
    R_WDT->WDTRR = 0xFFu;
    s_feeds++;
}

bool wdt_running(void)
{
    return s_running;
}

uint16_t wdt_counter(void)
{
    return (uint16_t)(R_WDT->WDTSR & 0x3FFFu);
}

uint32_t wdt_feeds(void)
{
    return s_feeds;
}
