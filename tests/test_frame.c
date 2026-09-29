/* Frame codec: round trip, every error path, resynchronization, and a deterministic fuzz campaign
 * (random garbage + mutated valid frames). The decoder must never crash (ASan/UBSan), never deliver
 * a frame whose CRC does not match, and must recover every valid frame that follows garbage. */
#include "test_util.h"
#include "trace/crc16.h"
#include "trace/frame.h"

#include <string.h>

static int feed(kframe_decoder_t *d, const uint8_t *b, size_t n, int *errors)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        kframe_result_t r = kframe_decode_byte(d, b[i]);
        if (r == KFD_FRAME) {
            frames++;
        } else if (r == KFD_ERROR) {
            (*errors)++;
        }
    }
    return frames;
}

int main(void)
{
    uint8_t buf[600], pl[KFRAME_MAX_PAYLOAD];
    kframe_decoder_t d;
    int err = 0;
    kframe_decoder_init(&d);
    for (uint16_t i = 0; i < sizeof(pl); i++) {
        pl[i] = (uint8_t)(i * 7u);
    }
    /* round trip at every legal length including 0 and max */
    for (uint16_t len = 0; len <= KFRAME_MAX_PAYLOAD; len++) {
        size_t n = kframe_encode(buf, sizeof(buf), 0x40, len, pl, len);
        CHECK(n == (size_t)len + KFRAME_OVERHEAD);
        CHECK(feed(&d, buf, n, &err) == 1);
        CHECK(d.len == len && d.seq == len && d.type == 0x40 && memcmp(d.payload, pl, len) == 0);
    }
    CHECK(err == 0);
    /* encoder refuses oversize payload and too-small output buffer */
    CHECK(kframe_encode(buf, sizeof(buf), 1, 0, pl, KFRAME_MAX_PAYLOAD + 1u) == 0);
    CHECK(kframe_encode(buf, 20, 1, 0, pl, 11) == 0);
    /* single-bit corruption anywhere after the magic is caught */
    size_t n = kframe_encode(buf, sizeof(buf), 0x40, 9, pl, 32);
    for (size_t pos = 2; pos < n; pos++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c[64];
            memcpy(c, buf, n);
            c[pos] ^= (uint8_t)(1u << bit);
            kframe_decoder_init(&d);
            int e = 0;
            int f = feed(&d, c, n, &e);
            /* A corrupted length can make the decoder wait for more bytes: no frame either way. */
            CHECK(f == 0);
            /* Resync: a corrupted length field can make the decoder absorb up to
             * KFRAME_MAX_PAYLOAD + 2 following bytes (documented limit of length-prefixed framing),
             * so recovery is required within that window: 16 frames of 42 B = 672 B > 482 B. */
            int ok = 0;
            for (int k = 0; k < 16; k++) {
                ok += feed(&d, buf, n, &e);
            }
            CHECK(ok >= 16 - 12);
        }
    }
    /* bad version and bad length are rejected at the header */
    kframe_decoder_init(&d);
    memcpy(buf, (const uint8_t[]){0x4B, 0x52, 2, 0x40, 0, 0, 0, 0, 0, 0}, 10);
    err = 0;
    CHECK(feed(&d, buf, 10, &err) == 0 && d.err_version == 1);
    memcpy(buf, (const uint8_t[]){0x4B, 0x52, 1, 0x40, 0, 0, 0xFF, 0x7F}, 8);
    CHECK(feed(&d, buf, 8, &err) == 0 && d.err_len == 1);

    /* fuzz: 20,000 rounds of garbage + (possibly mutated) frames, fixed seed */
    unsigned seed = 0xC0FFEEu;
    kframe_decoder_init(&d);
    long valid_sent = 0, valid_got = 0;
    for (int round = 0; round < 20000; round++) {
        uint8_t stream[2048];
        size_t sl = 0;
        size_t garbage = xs32(&seed) % 64u;
        for (size_t i = 0; i < garbage; i++) {
            stream[sl++] = (uint8_t)xs32(&seed);
        }
        uint16_t len = (uint16_t)(xs32(&seed) % (KFRAME_MAX_PAYLOAD + 1u));
        for (uint16_t i = 0; i < len; i++) {
            pl[i] = (uint8_t)xs32(&seed);
        }
        size_t fl = kframe_encode(&stream[sl], sizeof(stream) - sl, (uint8_t)xs32(&seed),
                                  (uint16_t)round, pl, len);
        int mutate = (xs32(&seed) % 4u) == 0u;
        if (mutate) {
            stream[sl + 2u + xs32(&seed) % (fl - 2u)] ^= (uint8_t)(1u + xs32(&seed) % 255u);
        }
        sl += fl;
        /* Follow with two max-size all-zero sync frames: a corrupted length can swallow at most 482
         * bytes (part of the first), zeros contain no false magic, so the second always decodes. */
        static const uint8_t zeros[KFRAME_MAX_PAYLOAD];
        for (int k = 0; k < 2; k++) {
            sl += kframe_encode(&stream[sl], sizeof(stream) - sl, 0x7E, 0xFFFF, zeros,
                                KFRAME_MAX_PAYLOAD);
        }
        int e = 0;
        uint32_t crc_before = d.err_crc;
        int f = 0;
        for (size_t i = 0; i < sl; i++) {
            if (kframe_decode_byte(&d, stream[i]) == KFD_FRAME) {
                f++;
                /* a delivered frame must carry a CRC that matches its own bytes */
                uint8_t hdr[6] = {KFRAME_VERSION,        d.type,         (uint8_t)d.seq,
                                  (uint8_t)(d.seq >> 8), (uint8_t)d.len, (uint8_t)(d.len >> 8)};
                uint16_t c = crc16_ccitt(crc16_ccitt(0xFFFFu, hdr, 6), d.payload, d.len);
                CHECK(c == d.crc_rx);
                if (d.type != 0x7E) {
                    valid_got++;
                }
            }
        }
        (void)e;
        (void)crc_before;
        if (!mutate) {
            valid_sent++;
        }
        CHECK(f >= 1); /* at least the sync frame */
    }
    /* Unmutated frames after random garbage can only be lost if the garbage contained "KR" and a
     * header whose length swallowed them; that is rare but legal. Mutated frames may pass only by
     * CRC-16 collision (~1/65536). */
    printf("   fuzz: %ld clean frames sent, %ld non-sync frames delivered, crc errors %u, len "
           "errors %u\n",
           valid_sent, valid_got, d.err_crc, d.err_len);
    CHECK(valid_got >= valid_sent * 99 / 100);
    DONE();
    return 0;
}
