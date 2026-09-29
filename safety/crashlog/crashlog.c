#include "safety/crashlog/crashlog.h"

#include "arch/armv7m/cpu.h"

#define VB_RECORD     128u
#define VB_BOOT       256u        /* u32 magic, u32 boot count */
#define VB_BOOT_MAGIC 0x4B424F54u /* "KBOT" */

static kcrash_boot_info_t s_info;
static kcrash_record_t s_pending;
static volatile uint32_t s_pending_seq; /* incremented per deferred store */
static uint32_t s_persisted_seq;

static void vb_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = R_SYSTEM->VBTBKR[off + i];
    }
}

static void vb_write(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    ra_prcr_unlock(KESTREL_PRCR_PRC1);
    for (uint32_t i = 0; i < n; i++) {
        R_SYSTEM->VBTBKR[off + i] = s[i];
    }
    ra_prcr_lock();
}

static uint32_t read_reset_cause(void)
{
    uint8_t r0 = R_SYSTEM->RSTSR0;
    uint16_t r1 = R_SYSTEM->RSTSR1;
    uint8_t r2 = R_SYSTEM->RSTSR2;
    uint32_t c = 0u;
    if ((r0 & 0x01u) != 0u) {
        c |= KRST_POWER_ON;
    }
    if ((r0 & 0x0Eu) != 0u) {
        c |= KRST_LVD;
    }
    if ((r1 & 0x0002u) != 0u) {
        c |= KRST_WDT;
    }
    if ((r1 & 0x0001u) != 0u) {
        c |= KRST_IWDT;
    }
    if ((r1 & 0x0004u) != 0u) {
        c |= KRST_SOFTWARE;
    }
    if ((r1 & 0x1000u) != 0u) {
        c |= KRST_SP_ERR;
    }
    if ((r1 & 0x0300u) != 0u) {
        c |= KRST_RAM_ERR;
    }
    if ((r1 & 0x0C00u) != 0u) {
        c |= KRST_BUS_ERR;
    }
    if ((r2 & 0x01u) != 0u) {
        c |= KRST_WARM;
    }
    if ((c & ~KRST_WARM) == 0u) {
        c |= KRST_PIN_OR_UNKNOWN; /* RES pin (bridge reset) leaves no dedicated flag */
    }
    /* Clear for the next boot: flags clear by writing 0; CWSF is set by writing 1 (warm marker). */
    ra_prcr_unlock(KESTREL_PRCR_PRC1);
    R_SYSTEM->RSTSR0 = 0u;
    R_SYSTEM->RSTSR1 = 0u;
    R_SYSTEM->RSTSR2 = 1u;
    ra_prcr_lock();
    return c;
}

void crashlog_boot(void)
{
    s_info.reset_cause = read_reset_cause();
    uint32_t hdr[2];
    vb_read(VB_BOOT, hdr, sizeof(hdr));
    if (hdr[0] != VB_BOOT_MAGIC) {
        hdr[0] = VB_BOOT_MAGIC;
        hdr[1] = 0u;
    }
    hdr[1]++;
    vb_write(VB_BOOT, hdr, sizeof(hdr));
    s_info.boot_count = hdr[1];

    vb_read(VB_RECORD, &s_info.last, sizeof(s_info.last));
    s_info.crash_present = s_info.last.magic == KCRASH_MAGIC &&
                           s_info.last.version == KCRASH_VERSION &&
                           s_info.last.size == sizeof(kcrash_record_t) &&
                           s_info.last.crc == crc32_ieee((const uint8_t *)&s_info.last,
                                                         (uint32_t)(sizeof(kcrash_record_t) - 4u));
}

const kcrash_boot_info_t *crashlog_info(void)
{
    return &s_info;
}

void crashlog_store(kcrash_record_t *r, bool durable_now)
{
    r->magic = KCRASH_MAGIC;
    r->version = KCRASH_VERSION;
    r->size = (uint16_t)sizeof(kcrash_record_t);
    r->boot_count = s_info.boot_count;
    r->crc = crc32_ieee((const uint8_t *)r, (uint32_t)(sizeof(kcrash_record_t) - 4u));
    s_info.last = *r; /* retrievable immediately, without a reboot */
    s_info.crash_present = true;
    if (durable_now) {
        vb_write(VB_RECORD, r, sizeof(*r));
    } else {
        s_pending = *r;
        s_pending_seq++;
    }
}

bool crashlog_persist(void)
{
    uint32_t seq = s_pending_seq;
    if (seq == s_persisted_seq) {
        return false;
    }
    kcrash_record_t copy;
    kcrit_t s = kcrit_enter();
    copy = s_pending; /* short: 124-byte RAM copy */
    kcrit_exit(s);
    vb_write(VB_RECORD, &copy, sizeof(copy)); /* ~1.1 ms, preemptible */
    s_persisted_seq = seq;
    return true;
}

uint32_t crashlog_pending(void)
{
    return s_pending_seq - s_persisted_seq;
}

void crashlog_clear(void)
{
    uint32_t zero[2] = {0u, 0u};
    vb_write(VB_RECORD, zero, sizeof(zero));
    s_info.crash_present = false;
}
