/* Phase 2 mailbox protocol: codec + RA4 receive policy (the trust boundary). */
#include <string.h>

#include "link/xlink.h"
#include "test_util.h"

static xl_decoder_t D;

static int decode_all(const uint8_t *b, size_t n, xl_frame_t *out)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        if (xl_decode_byte(&D, b[i]) == XLD_FRAME) {
            *out = D.f;
            frames++;
        }
    }
    return frames;
}

static xl_frame_t mk(uint8_t type, uint16_t session, uint32_t seq, const uint8_t *p, uint16_t len)
{
    uint8_t buf[300];
    xl_frame_t f;
    size_t n = xl_encode(buf, sizeof(buf), type, session, seq, p, len);
    CHECK(n == (size_t)len + XL_OVERHEAD);
    xl_decoder_init(&D);
    CHECK(decode_all(buf, n, &f) == 1);
    return f;
}

static xl_frame_t hello(uint32_t epoch, uint16_t session, uint32_t seq)
{
    uint8_t p[8];
    xl_put32(p, epoch);
    xl_put32(&p[4], 7u);
    return mk(XL_HELLO, session, seq, p, 8);
}

static xl_frame_t ping(uint16_t session, uint32_t seq)
{
    uint8_t p[4] = {1, 2, 3, 4};
    return mk(XL_PING, session, seq, p, 4);
}

int main(void)
{
    uint8_t buf[300], pl[XL_MAX_PAYLOAD];
    xl_frame_t f;
    for (uint16_t i = 0; i < XL_MAX_PAYLOAD; i++) {
        pl[i] = (uint8_t)(i * 13u);
    }
    /* codec round trip at every length */
    for (uint16_t len = 0; len <= XL_MAX_PAYLOAD; len++) {
        size_t n = xl_encode(buf, sizeof(buf), 0x42, 0xBEEF, 0x12345678u + len, pl, len);
        xl_decoder_init(&D);
        CHECK(decode_all(buf, n, &f) == 1);
        CHECK(f.type == 0x42 && f.session == 0xBEEF && f.seq == 0x12345678u + len && f.len == len);
        CHECK(memcmp(f.payload, pl, len) == 0);
    }
    CHECK(xl_encode(buf, sizeof(buf), 1, 1, 1, pl, XL_MAX_PAYLOAD + 1u) == 0);
    /* every single-bit corruption after the magic is rejected */
    size_t n = xl_encode(buf, sizeof(buf), XL_PING, 5, 9, pl, 4);
    for (size_t pos = 2; pos < n; pos++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c[64];
            memcpy(c, buf, n);
            c[pos] ^= (uint8_t)(1u << bit);
            xl_decoder_init(&D);
            CHECK(decode_all(c, n, &f) == 0);
        }
    }
    /* oversized length is rejected at the header, before any payload is buffered */
    uint8_t big[] = {0x4B, 0x58, 1, 2, 0, 0, 1, 0, 0, 0, 0x01, 0x10};
    xl_decoder_init(&D);
    CHECK(decode_all(big, sizeof(big), &f) == 0 && D.err_len == 1);

    /* ---- policy ---- */
    xl_rx_t r;
    uint32_t t = 1000;
    xl_rx_init(&r, 0xE9001234u, t);
    f = ping(7, 2);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_NO_SESSION); /* nothing before a HELLO */
    f = hello(0xDEADu, 7, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_EPOCH); /* HELLO from another RA4 boot */
    f = hello(0xE9001234u, 7, 2);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_HELLO_SEQ);
    f = hello(0xE9001234u, 0, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_RETIRED); /* session 0 is never valid */
    f = hello(0xE9001234u, 7, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_NEW_SESSION && r.session == 7);
    f = ping(7, 2);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_ACCEPT);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_REPLAY); /* exact replay */
    f = ping(7, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_REPLAY); /* older frame */
    f = ping(7, 2 + XL_MAX_SEQ_JUMP + 1u);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_JUMP);
    f = ping(9, 3);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_SESSION); /* other session */
    /* ESP reboots: new session; the old session and its frames are dead for this RA4 epoch */
    f = hello(0xE9001234u, 9, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_NEW_SESSION && r.session == 9);
    f = ping(7, 3);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_SESSION); /* stale pre-reboot traffic */
    f = hello(0xE9001234u, 7, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_RETIRED); /* replayed old HELLO cannot revive session 7 */
    /* type and length checks come first */
    f = mk(0x81, 9, 5, pl, 4); /* an RA4->ESP type sent to the RA4 */
    CHECK(xl_rx_accept(&r, &f, t) == XLR_BAD_TYPE);
    f = mk(XL_PING, 9, 5, pl, 5);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_BAD_LEN);
    /* rate limit: a burst larger than XL_RATE_BURST in the same millisecond is cut */
    xl_rx_init(&r, 1u, t);
    f = hello(1u, 3, 1);
    CHECK(xl_rx_accept(&r, &f, t) == XLR_NEW_SESSION);
    uint32_t ok = 0, rate = 0;
    for (uint32_t s = 2; s < 2 + 100; s++) {
        f = ping(3, s);
        xl_verdict_t v = xl_rx_accept(&r, &f, t);
        ok += v == XLR_ACCEPT;
        rate += v == XLR_RATE;
    }
    CHECK(ok == XL_RATE_BURST - 1u && rate == 100u - ok);
    /* ... and refills over time: 400/s */
    t += 1000;
    f = ping(3, 102);
    xl_verdict_t v2 = xl_rx_accept(&r, &f, t);
    if (v2 != XLR_ACCEPT) {
        printf("   refill verdict %d last_seq %u tokens %u\n", v2, r.last_seq, r.tokens_milli);
    }
    CHECK(v2 == XLR_ACCEPT); /* last accepted seq 40 -> 102 is a jump of 62 <= 64 */
    /* config whitelist: only noncritical ids exist */
    CHECK(xl_config_valid(XL_PARAM_LOGGER_UNITS, 20) && !xl_config_valid(XL_PARAM_LOGGER_UNITS, 0));
    CHECK(!xl_config_valid(XL_PARAM_HEALTH_UNITS, 201) && xl_config_valid(XL_PARAM_METRICS_MS, 1000));
    for (uint32_t id = 0; id < 256; id++) {
        if (id < 1 || id > 3) {
            CHECK(!xl_config_valid((uint8_t)id, 1));
        }
    }

    /* ---- fuzz: random bytes and mutated valid frames through decoder + policy ---- */
    unsigned seed = 0x5EEDu;
    xl_rx_init(&r, 0xABCDu, 0);
    uint32_t delivered = 0, accepted = 0;
    xl_decoder_init(&D);
    for (int round = 0; round < 200000; round++) {
        uint8_t s[64];
        size_t sl;
        if ((xs32(&seed) & 3u) == 0u) {
            sl = xs32(&seed) % sizeof(s);
            for (size_t i = 0; i < sl; i++) {
                s[i] = (uint8_t)xs32(&seed);
            }
        } else {
            uint8_t p[8];
            for (int i = 0; i < 8; i++) {
                p[i] = (uint8_t)xs32(&seed);
            }
            uint8_t types[] = {XL_HELLO, XL_PING, XL_CONFIG_REQ, XL_TELEM_CTRL, XL_TIME_SYNC, 0x81, 0x00, 0xFF};
            sl = xl_encode(s, sizeof(s), types[xs32(&seed) % 8u], (uint16_t)(xs32(&seed) % 4u),
                           xs32(&seed) % 70u, p, (uint16_t)(xs32(&seed) % 9u));
            if ((xs32(&seed) & 1u) != 0u && sl > 2) {
                s[2 + xs32(&seed) % (sl - 2)] ^= (uint8_t)(1u + xs32(&seed) % 255u);
            }
        }
        for (size_t i = 0; i < sl; i++) {
            if (xl_decode_byte(&D, s[i]) == XLD_FRAME) {
                delivered++;
                xl_verdict_t v = xl_rx_accept(&r, &D.f, (uint32_t)round);
                accepted += (v == XLR_ACCEPT || v == XLR_NEW_SESSION);
                CHECK(D.f.len <= XL_MAX_PAYLOAD);
            }
        }
    }
    /* No HELLO in the fuzz can carry the right epoch (random payload): no session, nothing accepted. */
    CHECK(r.session == 0u && accepted == 0u);
    printf("   fuzz: %u frames decoded, %u accepted (expected 0: no valid epoch), crc errs %u\n", delivered,
           accepted, D.err_crc);

    /* ---- fuzz phase 2: inside a valid session, mixed valid/invalid/replayed traffic ---- */
    xl_rx_init(&r, 0x77u, 0);
    f = hello(0x77u, 100, 1);
    CHECK(xl_rx_accept(&r, &f, 0) == XLR_NEW_SESSION);
    uint32_t prev_seq = r.last_seq, prev_session = r.session, new_sessions = 0, acc2 = 0;
    for (uint32_t k = 0; k < 300000; k++) {
        uint32_t now = k / 2u; /* ~2 frames per ms: above the rate limit, so it engages */
        uint16_t sess = (uint16_t)((xs32(&seed) % 8u == 0u) ? xs32(&seed) % 200u : r.session);
        uint32_t seq = r.last_seq + (xs32(&seed) % 80u) - 10u; /* replays, repeats, small and large jumps */
        uint8_t kind = (uint8_t)(xs32(&seed) % 10u);
        if (kind == 0u) {
            f = hello((xs32(&seed) & 1u) ? 0x77u : 0x66u, (uint16_t)(xs32(&seed) % 300u), (xs32(&seed) & 1u) ? 1u : seq);
        } else {
            f = ping(sess, seq);
        }
        xl_verdict_t v = xl_rx_accept(&r, &f, now);
        if (v == XLR_NEW_SESSION) {
            CHECK(f.type == XL_HELLO && xl_get32(f.payload) == 0x77u && f.seq == 1u);
            CHECK(f.session != prev_session && f.session != 0u);
            new_sessions++;
        } else if (v == XLR_ACCEPT) {
            CHECK(f.session == prev_session && f.seq > prev_seq && f.seq - prev_seq <= XL_MAX_SEQ_JUMP);
            acc2++;
        } else {
            CHECK(r.session == prev_session && r.last_seq == prev_seq); /* rejects change nothing */
        }
        prev_seq = r.last_seq;
        prev_session = r.session;
    }
    printf("   in-session fuzz: %u accepted, %u new sessions, rate-limited %u, replay %u, jump %u, stale session %u\n",
           acc2, new_sessions, r.rej_rate, r.rej_replay, r.rej_jump, r.rej_session);
    DONE();
    return 0;
}
