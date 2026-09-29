/* Real concurrency: one producer thread, one consumer thread, 2M elements through an 16-slot
 * queue. Checks ordering and that every element arrives exactly once; built with TSan. */
#include "kernel/ipc/spsc.h"
#include "test_util.h"

#include <pthread.h>

#define N 2000000u
static uint32_t st[16];
static kq_spsc_t q = {0, 0, 15, sizeof(uint32_t), (uint8_t *)st, 0, 0};

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < N;) {
        if (kq_spsc_push(&q, &i)) {
            i++;
        }
    }
    return 0;
}

int main(void)
{
    kq_spsc_reset(&q, 0xFFFFF000u);
    pthread_t th;
    pthread_create(&th, 0, producer, 0);
    uint32_t expect = 0, v;
    while (expect < N) {
        if (kq_spsc_pop(&q, &v)) {
            CHECK(v == expect);
            expect++;
        }
    }
    pthread_join(th, 0);
    CHECK(!kq_spsc_pop(&q, &v));
    printf("   %u elements in order, %u producer rejects (expected under contention)\n", N,
           atomic_load(&q.rejected));
    DONE();
    return 0;
}
