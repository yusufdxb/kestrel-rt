/* Kestrel link frame (both directions), little-endian:
 *
 *   off size field
 *   0   2    magic 0x4B 0x52 ("KR")
 *   2   1    version (KFRAME_VERSION)
 *   3   1    type
 *   4   2    sequence (per direction, wraps)
 *   6   2    payload length N (<= KFRAME_MAX_PAYLOAD)
 *   8   N    payload
 *   8+N 2    CRC-16/CCITT-FALSE over bytes [2, 8+N)  (magic excluded)
 *
 * The decoder is a byte-at-a-time state machine with bounded state: it never allocates, never
 * reads past its buffer, and resynchronizes on the next magic after any error. Portable and
 * fuzz-tested on the host. */
#ifndef KESTREL_FRAME_H
#define KESTREL_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KFRAME_MAGIC0      0x4Bu
#define KFRAME_MAGIC1      0x52u
#define KFRAME_VERSION     1u
#define KFRAME_HEADER      8u
#define KFRAME_OVERHEAD    10u
#define KFRAME_MAX_PAYLOAD 480u

/* Target -> host */
#define KF_HELLO  0x01u
#define KF_STATUS 0x02u
#define KF_JOBS   0x03u
#define KF_TRACE  0x04u
#define KF_CRASH  0x05u
#define KF_ACK    0x06u
#define KF_TASKS  0x07u
#define KF_RESULT 0x08u
/* Host -> target */
#define KF_CMD 0x40u

/* Encode into out (capacity cap). Returns bytes written, 0 if it does not fit or len too big. */
size_t kframe_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t seq, const uint8_t *payload,
                     uint16_t len);

typedef enum {
    KFD_NONE = 0,  /* need more bytes */
    KFD_FRAME = 1, /* complete, valid frame available in the decoder */
    KFD_ERROR = 2, /* frame discarded (see counters); decoder already resynchronizing */
} kframe_result_t;

typedef struct {
    uint8_t state;
    uint16_t idx;
    uint16_t len;
    uint8_t type;
    uint8_t version;
    uint16_t seq;
    uint16_t crc_rx;
    uint8_t crc_lo;
    uint8_t payload[KFRAME_MAX_PAYLOAD];
    /* counters */
    uint32_t frames_ok;
    uint32_t err_crc;
    uint32_t err_len;
    uint32_t err_version;
    uint32_t bytes_skipped;
} kframe_decoder_t;

void kframe_decoder_init(kframe_decoder_t *d);
kframe_result_t kframe_decode_byte(kframe_decoder_t *d, uint8_t b);

static inline void kput16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void kput32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t kget16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}
static inline uint32_t kget32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif
