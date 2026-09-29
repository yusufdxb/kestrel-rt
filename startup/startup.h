#ifndef KESTREL_STARTUP_H
#define KESTREL_STARTUP_H
#include <stdint.h>

#define KESTREL_NUM_IRQ 32u

typedef struct {
    uint32_t sckdivcr;
    uint8_t sckscr;
    uint8_t memwait;
    uint8_t opccr;
    uint8_t hocosf;
} kestrel_clock_state_t;

/* Configure HOCO 48 MHz: ICLK 48, PCLKA 48, PCLKB 24, PCLKC 48, PCLKD 48, FCLK 24 MHz. */
void kestrel_clock_init(void);
void kestrel_clock_read(kestrel_clock_state_t *out);

/* Kernel entry after C runtime is up (kernel/boot.c). */
void kestrel_boot(void) __attribute__((noreturn));
#endif
