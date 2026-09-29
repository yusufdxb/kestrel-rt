#include "safety/crashlog/crashlog.h"

/* CRC-32/ISO-HDLC (IEEE 802.3), bitwise: small, portable, only used for 124-byte records. */
uint32_t crc32_ieee(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint32_t k = 0; k < 8u; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}
