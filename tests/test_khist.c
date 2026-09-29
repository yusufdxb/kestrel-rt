#include "kernel/khist.h"
#include "test_util.h"

khist_t g_hist_pendsv, g_hist_alarm, g_hist_svc, g_hist_irqlat;

int main(void)
{
    khist_t h = {2u, 0u, 4u, 0, 0, 0, {0}};
    for (uint32_t v = 0; v < 1000; v++) {
        khist_add(&h, v);
    }
    CHECK(h.count == 1000 && h.max == 999);
    CHECK(h.bins[0] == 4 && h.bins[127] == 4); /* 128 bins x width 4 = 0..511 */
    CHECK(h.overflow == 1000 - 512);
    khist_reset(&h);
    CHECK(h.count == 0 && h.bins[5] == 0 && h.overflow == 0);
    DONE();
    return 0;
}
