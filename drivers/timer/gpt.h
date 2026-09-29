/* GPT1 (32-bit) as a programmable interrupt source for the interrupt-storm campaign. Its ISR does
 * a fixed, tiny amount of work (counter increment) at KPRIO_STORM, above every task. */
#ifndef KESTREL_GPT_H
#define KESTREL_GPT_H
#include <stdint.h>

void storm_set_rate(uint32_t hz); /* 0 = off */
uint32_t storm_count(void);
#endif
