/* RA4M1 WDT in register-start mode (OFS0.WDTSTRT = 1 in the factory/bootloader option bytes, so
 * the WDT is idle after reset until Kestrel configures and first refreshes it). Once started it
 * cannot be stopped until the next reset. Clock: PCLKB 24 MHz / 8192, timeout 1024 counts
 * = 349.5 ms, no refresh window, underflow => reset (RSTIRQS = 1). */
#ifndef KESTREL_WDT_H
#define KESTREL_WDT_H
#include <stdbool.h>
#include <stdint.h>

#define KWDT_TIMEOUT_US 349525u

typedef enum { WDT_OK = 0, WDT_EALREADY = -1 } wdt_status_t;

wdt_status_t wdt_start(void);
void wdt_feed(void);
bool wdt_running(void);
uint16_t wdt_counter(void);
uint32_t wdt_feeds(void);
#endif
