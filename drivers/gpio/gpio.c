#include "drivers/gpio/gpio.h"

#include "arch/armv7m/cpu.h"

#define PFS_PMR   (1u << 16)
#define PFS_PDR   (1u << 2)
#define PFS_PCR   (1u << 4)
#define PFS_PODR  (1u << 0)
#define PFS_PSEL_SHIFT 24u

static R_PORT0_Type *port_regs(uint32_t port)
{
    return (R_PORT0_Type *)(R_PORT0_BASE + port * (R_PORT1_BASE - R_PORT0_BASE));
}

static volatile uint32_t *pfs_reg(uint16_t pin)
{
    uint32_t port = (uint32_t)pin >> 8;
    uint32_t bit = (uint32_t)pin & 0xFFu;
    return &R_PFS->PORT[port].PIN[bit].PmnPFS;
}

static bool valid(uint16_t pin)
{
    return ((uint32_t)pin >> 8) <= 9u && ((uint32_t)pin & 0xFFu) < 16u;
}

static void pfs_write(uint16_t pin, uint32_t value)
{
    kcrit_t s = kcrit_enter();
    R_PMISC->PWPR = 0u;          /* B0WI = 0 */
    R_PMISC->PWPR = 0x40u;       /* PFSWE = 1 */
    *pfs_reg(pin) = value;
    R_PMISC->PWPR = 0u;
    R_PMISC->PWPR = 0x80u;       /* B0WI = 1 */
    kcrit_exit(s);
}

gpio_status_t gpio_init_output(uint16_t pin, bool initial)
{
    if (!valid(pin)) {
        return GPIO_EINVAL;
    }
    if (pin == GPIO_USB_MUX) {
        return GPIO_EFORBIDDEN;
    }
    pfs_write(pin, PFS_PDR | (initial ? PFS_PODR : 0u));
    return GPIO_OK;
}

gpio_status_t gpio_init_input(uint16_t pin, bool pullup)
{
    if (!valid(pin)) {
        return GPIO_EINVAL;
    }
    pfs_write(pin, pullup ? PFS_PCR : 0u);
    return GPIO_OK;
}

gpio_status_t gpio_set_peripheral(uint16_t pin, uint32_t psel)
{
    if (!valid(pin) || psel > 0x1Fu) {
        return GPIO_EINVAL;
    }
    if (pin == GPIO_USB_MUX) {
        return GPIO_EFORBIDDEN;
    }
    pfs_write(pin, 0u); /* PMR must be 0 while PSEL changes */
    pfs_write(pin, (psel << PFS_PSEL_SHIFT));
    pfs_write(pin, (psel << PFS_PSEL_SHIFT) | PFS_PMR);
    return GPIO_OK;
}

void gpio_write(uint16_t pin, bool level)
{
    uint32_t bit = 1u << ((uint32_t)pin & 0xFFu);
    /* PCNTR3: lower half POSR (set), upper half PORR (reset); single write, no read-modify-write. */
    port_regs((uint32_t)pin >> 8)->PCNTR3 = level ? bit : (bit << 16);
}

void gpio_toggle(uint16_t pin)
{
    uint32_t bit = 1u << ((uint32_t)pin & 0xFFu);
    R_PORT0_Type *p = port_regs((uint32_t)pin >> 8);
    kcrit_t s = kcrit_enter();
    uint32_t odr = p->PCNTR1 >> 16;
    p->PCNTR3 = ((odr & bit) != 0u) ? (bit << 16) : bit;
    kcrit_exit(s);
}

bool gpio_read(uint16_t pin)
{
    uint32_t bit = 1u << ((uint32_t)pin & 0xFFu);
    return (port_regs((uint32_t)pin >> 8)->PCNTR2 & bit) != 0u;
}
