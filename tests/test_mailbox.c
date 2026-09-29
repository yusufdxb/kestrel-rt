#include "kernel/ipc/mailbox.h"
#include "test_util.h"

typedef struct {
    uint32_t seq;
    float v[4];
} msg_t;

static msg_t data;
static kmailbox_t mb = {0, sizeof(msg_t), (volatile uint8_t *)&data, 0, 0};

int main(void)
{
    msg_t out;
    uint32_t s;
    CHECK(kmb_read(&mb, &out, &s) == KMB_EMPTY);
    for (uint32_t i = 1; i <= 1000; i++) {
        msg_t w = {i, {(float)i, 0, 0, 0}};
        kmb_write(&mb, &w);
        CHECK(kmb_read(&mb, &out, &s) == KMB_OK);
        CHECK(out.seq == i && s == 2u * i); /* latest value only; seq even, +2 per write */
    }
    /* A writer in progress (odd seq) makes readers retry, then report BUSY: bounded, never torn. */
    atomic_store(&mb.seq, atomic_load(&mb.seq) + 1u);
    CHECK(kmb_read(&mb, &out, &s) == KMB_BUSY);
    CHECK(atomic_load(&mb.read_retries) == KMB_MAX_RETRIES);
    CHECK(atomic_load(&mb.read_busy) == 1);
    DONE();
    return 0;
}
