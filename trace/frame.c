#include "trace/frame.h"

#include "trace/crc16.h"

enum { S_M0 = 0, S_M1, S_HDR, S_PAYLOAD, S_CRC };

size_t kframe_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t seq, const uint8_t *payload,
                     uint16_t len)
{
    if (len > KFRAME_MAX_PAYLOAD || cap < (size_t)len + KFRAME_OVERHEAD) {
        return 0u;
    }
    out[0] = KFRAME_MAGIC0;
    out[1] = KFRAME_MAGIC1;
    out[2] = KFRAME_VERSION;
    out[3] = type;
    kput16(&out[4], seq);
    kput16(&out[6], len);
    for (uint16_t i = 0; i < len; i++) {
        out[KFRAME_HEADER + i] = payload[i];
    }
    uint16_t crc = crc16_ccitt(0xFFFFu, &out[2], (size_t)len + 6u);
    kput16(&out[KFRAME_HEADER + len], crc);
    return (size_t)len + KFRAME_OVERHEAD;
}

void kframe_decoder_init(kframe_decoder_t *d)
{
    uint8_t *p = (uint8_t *)d;
    for (size_t i = 0; i < sizeof(*d); i++) {
        p[i] = 0u;
    }
    d->state = S_M0;
}

static kframe_result_t fail(kframe_decoder_t *d, uint32_t *counter)
{
    (*counter)++;
    d->state = S_M0;
    return KFD_ERROR;
}

kframe_result_t kframe_decode_byte(kframe_decoder_t *d, uint8_t b)
{
    switch (d->state) {
    case S_M0:
        if (b == KFRAME_MAGIC0) {
            d->state = S_M1;
        } else {
            d->bytes_skipped++;
        }
        return KFD_NONE;
    case S_M1:
        if (b == KFRAME_MAGIC1) {
            d->state = S_HDR;
            d->idx = 0u;
        } else if (b != KFRAME_MAGIC0) {
            d->bytes_skipped += 2u;
            d->state = S_M0;
        }
        return KFD_NONE;
    case S_HDR:
        /* header bytes 2..7 buffered in payload[0..5] temporarily */
        d->payload[d->idx++] = b;
        if (d->idx == 6u) {
            d->version = d->payload[0];
            d->type = d->payload[1];
            d->seq = kget16(&d->payload[2]);
            d->len = kget16(&d->payload[4]);
            d->crc_rx = crc16_ccitt(0xFFFFu, d->payload, 6u);
            if (d->version != KFRAME_VERSION) {
                return fail(d, &d->err_version);
            }
            if (d->len > KFRAME_MAX_PAYLOAD) {
                return fail(d, &d->err_len);
            }
            d->idx = 0u;
            d->state = (d->len == 0u) ? S_CRC : S_PAYLOAD;
        }
        return KFD_NONE;
    case S_PAYLOAD:
        d->payload[d->idx++] = b;
        d->crc_rx = crc16_ccitt(d->crc_rx, &b, 1u);
        if (d->idx == d->len) {
            d->idx = 0u;
            d->state = S_CRC;
        }
        return KFD_NONE;
    case S_CRC:
    default:
        if (d->idx == 0u) {
            d->crc_lo = b;
            d->idx = 1u;
            return KFD_NONE;
        }
        d->state = S_M0;
        if ((uint16_t)(d->crc_lo | (uint16_t)(b << 8)) != d->crc_rx) {
            d->err_crc++;
            return KFD_ERROR;
        }
        d->frames_ok++;
        return KFD_FRAME;
    }
}
