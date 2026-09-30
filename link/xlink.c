#include "link/xlink.h"

#include "trace/crc16.h"

enum { S_M0 = 0, S_M1, S_HDR, S_PAYLOAD, S_CRC0, S_CRC1 };

size_t xl_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t session, uint32_t seq,
                 const uint8_t *payload, uint16_t len)
{
    if (len > XL_MAX_PAYLOAD || cap < (size_t)len + XL_OVERHEAD) {
        return 0u;
    }
    out[0] = XL_MAGIC0;
    out[1] = XL_MAGIC1;
    out[2] = XL_VERSION;
    out[3] = type;
    xl_put16(&out[4], session);
    xl_put32(&out[6], seq);
    xl_put16(&out[10], len);
    for (uint16_t i = 0; i < len; i++) {
        out[XL_HEADER + i] = payload[i];
    }
    xl_put16(&out[XL_HEADER + len], crc16_ccitt(0xFFFFu, &out[2], (size_t)len + 10u));
    return (size_t)len + XL_OVERHEAD;
}

void xl_decoder_init(xl_decoder_t *d)
{
    uint8_t *p = (uint8_t *)d;
    for (size_t i = 0; i < sizeof(*d); i++) {
        p[i] = 0u;
    }
}

xl_dec_result_t xl_decode_byte(xl_decoder_t *d, uint8_t b)
{
    switch (d->state) {
    case S_M0:
        if (b == XL_MAGIC0) {
            d->state = S_M1;
        } else {
            d->bytes_skipped++;
        }
        return XLD_NONE;
    case S_M1:
        if (b == XL_MAGIC1) {
            d->state = S_HDR;
            d->idx = 0u;
        } else if (b != XL_MAGIC0) {
            d->bytes_skipped += 2u;
            d->state = S_M0;
        }
        return XLD_NONE;
    case S_HDR:
        d->hdr[d->idx++] = b;
        if (d->idx < 10u) {
            return XLD_NONE;
        }
        d->state = S_M0; /* default for the error paths below */
        if (d->hdr[0] != XL_VERSION) {
            d->err_version++;
            return XLD_ERROR;
        }
        d->f.type = d->hdr[1];
        d->f.session = xl_get16(&d->hdr[2]);
        d->f.seq = xl_get32(&d->hdr[4]);
        d->f.len = xl_get16(&d->hdr[8]);
        if (d->f.len > XL_MAX_PAYLOAD) {
            d->err_len++;
            return XLD_ERROR;
        }
        d->crc = crc16_ccitt(0xFFFFu, d->hdr, 10u);
        d->idx = 0u;
        d->state = (d->f.len == 0u) ? S_CRC0 : S_PAYLOAD;
        return XLD_NONE;
    case S_PAYLOAD:
        d->f.payload[d->idx++] = b;
        d->crc = crc16_ccitt(d->crc, &b, 1u);
        if (d->idx == d->f.len) {
            d->state = S_CRC0;
        }
        return XLD_NONE;
    case S_CRC0:
        d->crc_lo = b;
        d->state = S_CRC1;
        return XLD_NONE;
    case S_CRC1:
    default:
        d->state = S_M0;
        if ((uint16_t)(d->crc_lo | (uint16_t)(b << 8)) != d->crc) {
            d->err_crc++;
            return XLD_ERROR;
        }
        d->frames++;
        return XLD_FRAME;
    }
}

static int expected_len(uint8_t type)
{
    switch (type) {
    case XL_HELLO:
        return 8;
    case XL_PING:
        return 4;
    case XL_CONFIG_REQ:
        return 8;
    case XL_TELEM_CTRL:
        return 4;
    case XL_TIME_SYNC:
        return 8;
    default:
        return -1;
    }
}

void xl_rx_init(xl_rx_t *r, uint32_t epoch, uint32_t now_ms)
{
    uint8_t *p = (uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        p[i] = 0u;
    }
    r->epoch = epoch;
    r->tokens_milli = XL_RATE_BURST * 1000u;
    r->last_ms = now_ms;
}

static bool take_token(xl_rx_t *r, uint32_t now_ms)
{
    uint32_t dt = now_ms - r->last_ms;
    r->last_ms = now_ms;
    uint64_t t = (uint64_t)r->tokens_milli + (uint64_t)dt * XL_RATE_PER_S;
    uint64_t cap = (uint64_t)XL_RATE_BURST * 1000u;
    r->tokens_milli = (uint32_t)(t > cap ? cap : t);
    if (r->tokens_milli < 1000u) {
        return false;
    }
    r->tokens_milli -= 1000u;
    return true;
}

static bool retired(const xl_rx_t *r, uint16_t s)
{
    uint32_t n = r->n_retired < XL_RETIRED_SLOTS ? r->n_retired : XL_RETIRED_SLOTS;
    for (uint32_t i = 0; i < n; i++) {
        if (r->retired[i] == s) {
            return true;
        }
    }
    return false;
}

xl_verdict_t xl_rx_accept(xl_rx_t *r, const xl_frame_t *f, uint32_t now_ms)
{
    int want = expected_len(f->type);
    if (want < 0) {
        r->rej_type++;
        return XLR_BAD_TYPE;
    }
    if ((int)f->len != want) {
        r->rej_len++;
        return XLR_BAD_LEN;
    }
    if (!take_token(r, now_ms)) {
        r->rej_rate++;
        return XLR_RATE;
    }
    if (f->type == XL_HELLO) {
        if (xl_get32(f->payload) != r->epoch) {
            r->rej_epoch++;
            return XLR_EPOCH;
        }
        if (f->session == 0u || f->session == r->session || retired(r, f->session)) {
            r->rej_retired++;
            return XLR_RETIRED;
        }
        if (f->seq != 1u) {
            r->rej_hello_seq++;
            return XLR_HELLO_SEQ;
        }
        if (r->session != 0u) {
            r->retired[r->n_retired % XL_RETIRED_SLOTS] = r->session;
            r->n_retired++;
        }
        r->session = f->session;
        r->last_seq = 1u;
        r->sessions++;
        r->accepted++;
        return XLR_NEW_SESSION;
    }
    if (r->session == 0u) {
        r->rej_nosession++;
        return XLR_NO_SESSION;
    }
    if (f->session != r->session) {
        r->rej_session++;
        return XLR_SESSION;
    }
    if (f->seq <= r->last_seq) {
        r->rej_replay++;
        return XLR_REPLAY;
    }
    if (f->seq - r->last_seq > XL_MAX_SEQ_JUMP) {
        r->rej_jump++;
        return XLR_JUMP;
    }
    r->last_seq = f->seq;
    r->accepted++;
    return XLR_ACCEPT;
}

bool xl_config_valid(uint8_t param, uint32_t value)
{
    switch (param) {
    case XL_PARAM_LOGGER_UNITS:
        return value >= 1u && value <= 400u;
    case XL_PARAM_HEALTH_UNITS:
        return value >= 1u && value <= 200u;
    case XL_PARAM_METRICS_MS:
        return value >= 100u && value <= 5000u;
    default:
        return false; /* every other id, including anything critical, does not exist */
    }
}
