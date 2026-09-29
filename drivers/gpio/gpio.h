/* Register-level GPIO for RA4M1 ports (PCNTR1 direction/output, PCNTR3 atomic set/reset). */
#ifndef KESTREL_GPIO_H
#define KESTREL_GPIO_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { GPIO_OK = 0, GPIO_EINVAL = -1, GPIO_EFORBIDDEN = -2 } gpio_status_t;

#define GPIO_PIN(port, pin) ((uint16_t)(((port) << 8) | (pin)))
#define GPIO_LED_BUILTIN    GPIO_PIN(1u, 2u) /* P102, D13 "L" LED */
#define GPIO_USB_MUX        GPIO_PIN(4u, 8u) /* P408: never driven by Kestrel (cuts bridge + SWD) */

gpio_status_t gpio_init_output(uint16_t pin, bool initial);
gpio_status_t gpio_init_input(uint16_t pin, bool pullup);
void gpio_write(uint16_t pin, bool level);
void gpio_toggle(uint16_t pin);
bool gpio_read(uint16_t pin);
/* Peripheral function select: psel per RA4M1 PmnPFS.PSEL encoding. */
gpio_status_t gpio_set_peripheral(uint16_t pin, uint32_t psel);
#endif
