#include "kernel/ipc/spsc.h"
#include "test_util.h"

typedef struct {
    uint32_t a;
    uint16_t b;
} rec_t;

int main(void)
{
    rec_t st[8];
    kq_spsc_t q = {0, 0, 7, sizeof(rec_t), (uint8_t *)st, 0, 0};
    rec_t r;
    CHECK(!kq_spsc_pop(&q, &r));
    for (uint32_t i = 0; i < 8; i++) {
        rec_t w = {i, (uint16_t)(i * 3)};
        CHECK(kq_spsc_push(&q, &w));
    }
    rec_t x = {99, 99};
    CHECK(!kq_spsc_push(&q, &x)); /* full: REJECT policy */
    CHECK(atomic_load(&q.rejected) == 1);
    CHECK(atomic_load(&q.high_water) == 8);
    for (uint32_t i = 0; i < 8; i++) {
        CHECK(kq_spsc_pop(&q, &r));
        CHECK(r.a == i && r.b == i * 3); /* FIFO, rejected element never appears */
    }
    CHECK(!kq_spsc_pop(&q, &r));

    /* index wrap across 2^32: start just below the wrap and stream through it */
    kq_spsc_reset(&q, 0xFFFFFFF0u);
    uint32_t next_in = 0, next_out = 0;
    for (uint32_t step = 0; step < 100000; step++) {
        if ((step % 3) != 2) {
            rec_t w = {next_in, 0};
            if (kq_spsc_push(&q, &w)) {
                next_in++;
            }
        } else {
            while (kq_spsc_pop(&q, &r)) {
                CHECK(r.a == next_out);
                next_out++;
            }
        }
        CHECK(kq_spsc_count(&q) <= 8);
    }
    CHECK(atomic_load(&q.head) < 0xFFFFFFF0u); /* really wrapped */
    DONE();
    return 0;
}
