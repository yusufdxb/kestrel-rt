/* Phase 2 interprocessor mailbox protocol (RA4M1 <-> ESP32-S3), portable C (RA4, ESP fork, host).
 *
 *   off size field
 *   0   2    magic 0x4B 0x58 ("KX")
 *   2   1    version (XL_VERSION)
 *   3   1    type
 *   4   2    session id of the SENDER's session
 *   6   4    sequence (per direction and session, strictly increasing, first frame = 1)
 *   10  2    payload length N (<= XL_MAX_PAYLOAD)
 *   12  N    payload (little endian fields)
 *   12+N 2   CRC-16/CCITT-FALSE over bytes [2, 12+N)
 *
 * The ESP32-S3 is untrusted. The RA4 side (xl_rx_*) accepts a frame only if the frame decodes, the
 * type is an ESP->RA4 type with exactly its payload length, the rate limit allows it, and the
 * session/sequence rules hold (see xl_rx_accept). No frame can name a function, an address, a
 * register, a priority or a critical parameter: CONFIG_REQUEST carries a parameter id that is
 * looked up in a fixed whitelist of noncritical settings. */
#ifndef KESTREL_XLINK_H
#define KESTREL_XLINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XL_MAGIC0      0x4Bu
#define XL_MAGIC1      0x58u
#define XL_VERSION     1u
#define XL_HEADER      12u
#define XL_OVERHEAD    14u
#define XL_MAX_PAYLOAD 256u

/* ESP -> RA4 */
#define XL_HELLO      0x01u /* u32 ra4_epoch_seen, u32 esp_boot_count */
#define XL_PING       0x02u /* u32 nonce */
#define XL_CONFIG_REQ 0x03u /* u8 param, u8 pad[3], u32 value */
#define XL_TELEM_CTRL 0x04u /* u32 class_mask */
#define XL_TIME_SYNC  0x05u /* u64 esp_time_us (metadata only) */
/* RA4 -> ESP */
#define XL_STATUS     0x81u
#define XL_TRACE      0x82u
#define XL_FAULT      0x83u
#define XL_METRICS    0x84u
#define XL_HEARTBEAT  0x85u
#define XL_PONG       0x86u
#define XL_CONFIG_ACK 0x87u

size_t xl_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t session, uint32_t seq,
                 const uint8_t *payload, uint16_t len);

typedef struct {
    uint8_t type;
    uint16_t session;
    uint32_t seq;
    uint16_t len;
    uint8_t payload[XL_MAX_PAYLOAD];
} xl_frame_t;

typedef enum { XLD_NONE = 0, XLD_FRAME = 1, XLD_ERROR = 2 } xl_dec_result_t;

typedef struct {
    uint8_t state;
    uint16_t idx;
    uint8_t hdr[10];
    uint16_t crc;
    uint8_t crc_lo;
    xl_frame_t f;
    uint32_t frames;
    uint32_t err_crc;
    uint32_t err_len;
    uint32_t err_version;
    uint32_t bytes_skipped;
} xl_decoder_t;

void xl_decoder_init(xl_decoder_t *d);
xl_dec_result_t xl_decode_byte(xl_decoder_t *d, uint8_t b);

/* ---- RA4 receive policy (the trust boundary) ---- */
typedef enum {
    XLR_ACCEPT = 0,
    XLR_NEW_SESSION = 1, /* accepted HELLO: a new session starts */
    XLR_BAD_TYPE = -1,
    XLR_BAD_LEN = -2,
    XLR_RATE = -3,
    XLR_NO_SESSION = -4,
    XLR_SESSION = -5, /* frame from another (stale) session */
    XLR_REPLAY = -6,  /* seq <= last accepted */
    XLR_JUMP = -7,    /* seq advanced by more than XL_MAX_SEQ_JUMP */
    XLR_EPOCH = -8,   /* HELLO does not echo this RA4 boot's epoch */
    XLR_RETIRED = -9, /* HELLO names a session id already used in this RA4 epoch */
    XLR_HELLO_SEQ = -10,
} xl_verdict_t;

#define XL_MAX_SEQ_JUMP  64u
#define XL_RETIRED_SLOTS 32u
#define XL_RATE_PER_S    400u /* token bucket refill */
#define XL_RATE_BURST    40u

typedef struct {
    uint32_t epoch;   /* RA4 boot epoch the ESP must echo in HELLO */
    uint16_t session; /* current accepted ESP session, 0 = none */
    uint32_t last_seq;
    uint16_t retired[XL_RETIRED_SLOTS];
    uint32_t n_retired;
    uint32_t tokens_milli; /* token bucket, 1000 = one frame */
    uint32_t last_ms;
    /* counters */
    uint32_t accepted, sessions;
    uint32_t rej_type, rej_len, rej_rate, rej_nosession, rej_session, rej_replay, rej_jump,
        rej_epoch, rej_retired, rej_hello_seq;
} xl_rx_t;

void xl_rx_init(xl_rx_t *r, uint32_t epoch, uint32_t now_ms);
xl_verdict_t xl_rx_accept(xl_rx_t *r, const xl_frame_t *f, uint32_t now_ms);

/* ---- noncritical configuration whitelist ---- */
#define XL_PARAM_LOGGER_UNITS 1u /* background work per loop, 1..400 */
#define XL_PARAM_HEALTH_UNITS 2u /* health work per job, 1..200 */
#define XL_PARAM_METRICS_MS   3u /* METRICS period, 100..5000 ms */
bool xl_config_valid(uint8_t param, uint32_t value);

static inline void xl_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void xl_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t xl_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}
static inline uint32_t xl_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif
