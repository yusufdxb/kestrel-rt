#include "safety/crashlog/crashlog.h"
#include "test_util.h"
#include "trace/crc16.h"

#include <string.h>

int main(void)
{
    const uint8_t *s = (const uint8_t *)"123456789";
    CHECK(crc16_ccitt(0xFFFFu, s, 9) == 0x29B1u); /* CRC-16/CCITT-FALSE check value */
    CHECK(crc32_ieee(s, 9) == 0xCBF43926u);       /* CRC-32/ISO-HDLC check value */
    CHECK(crc16_ccitt(crc16_ccitt(0xFFFFu, s, 4), s + 4, 5) == 0x29B1u); /* streaming */
    DONE();
    return 0;
}
