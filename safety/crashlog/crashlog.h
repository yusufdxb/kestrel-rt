/* Crash record that survives resets without touching flash.
 *
 * Storage: the RA4M1 VBATT backup registers (VBTBKR, 512 bytes, retained across every reset
 * except loss of VCC and VBATT). The Arduino bootloader uses bytes 0-3 (double-tap magic) and
 * nothing else; its SRAM use makes .noinit SRAM unsafe, which is why SRAM is not used. Kestrel
 * uses bytes 128-255 for the record and 256-287 for boot bookkeeping. Writes need PRCR.PRC1.
 * Zero flash wear: flash is never written at run time. */
#ifndef KESTREL_CRASHLOG_H
#define KESTREL_CRASHLOG_H
#include <stdbool.h>
#include <stdint.h>

#define KCRASH_MAGIC   0x4B435253u /* "KCRS" */
#define KCRASH_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t kind;          /* KFAULT_* */
    uint8_t task;           /* 0xFF = kernel/ISR context */
    uint8_t flags;          /* KCF_* */
    uint32_t boot_count;    /* boot on which the fault happened */
    uint32_t time_lo;       /* timebase ticks */
    uint32_t time_hi;
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr; /* stacked frame */
    uint32_t cfsr, hfsr, mmfar, bfar, shcsr, icsr;
    uint32_t exc_return;
    uint32_t sp;            /* frame address (PSP or MSP) */
    uint32_t detail;        /* kind-specific: assert line, stalled-task mask, ... */
    uint16_t stack_used;    /* high-water of the faulting task, bytes */
    uint16_t capture_cycles;/* entry-to-record latency, DWT cycles (saturating) */
    uint32_t crc;           /* CRC-32 over all preceding bytes */
} kcrash_record_t;

_Static_assert(sizeof(kcrash_record_t) <= 128u, "crash record must fit its VBTBKR window");

#define KCF_FRAME_VALID 0x01u /* stacked frame could be read */
#define KCF_STACK_ERR   0x02u /* fault during exception stacking (frame not trustworthy) */
#define KCF_RESET       0x04u /* policy decided to reset */
#define KCF_IN_HANDLER  0x08u /* fault in handler mode (kernel/ISR) */

typedef struct {
    uint32_t boot_count;
    uint32_t reset_cause;   /* KRST_* bits of this boot */
    bool crash_present;     /* valid record found at boot */
    kcrash_record_t last;   /* copy of that record */
} kcrash_boot_info_t;

#define KRST_POWER_ON  0x0001u
#define KRST_WDT       0x0002u
#define KRST_IWDT      0x0004u
#define KRST_SOFTWARE  0x0008u
#define KRST_LVD       0x0010u
#define KRST_SP_ERR    0x0020u
#define KRST_RAM_ERR   0x0040u
#define KRST_BUS_ERR   0x0080u
#define KRST_WARM      0x0100u
#define KRST_PIN_OR_UNKNOWN 0x0200u

void crashlog_boot(void);                       /* read reset cause + stored record, count boot */
const kcrash_boot_info_t *crashlog_info(void);
void crashlog_store(kcrash_record_t *r);        /* finalize (CRC) and write */
void crashlog_clear(void);
uint32_t crc32_ieee(const uint8_t *p, uint32_t n); /* portable */
#endif
