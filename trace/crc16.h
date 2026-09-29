/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0). Check value for
 * "123456789" is 0x29B1 (tested). Table-driven: 512 B of flash, ~6 cycles/byte on the M4. */
#ifndef KESTREL_CRC16_H
#define KESTREL_CRC16_H
#include <stddef.h>
#include <stdint.h>
uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len);
#endif
