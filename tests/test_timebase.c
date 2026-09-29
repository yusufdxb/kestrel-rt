/* 64-bit extension of the 32-bit GPT counter: every interleaving of "sample" and "wrap". */
#include "kernel/time/timebase.h"
#include "test_util.h"

int main(void)
{
    /* no pending overflow: plain concatenation */
    CHECK(kt_extend(5, 0x10u, false) == ((5ull << 32) | 0x10u));
    CHECK(kt_extend(5, 0xFFFFFFF0u, false) == ((5ull << 32) | 0xFFFFFFF0u));
    /* overflow pending and the (re-)sample is after the wrap: high word not yet incremented */
    CHECK(kt_extend(5, 0x3u, true) == ((6ull << 32) | 0x3u));
    /* overflow pending but sample is before the wrap point (cannot happen after the re-sample in
     * kt_now, but the helper must still be monotonic) */
    CHECK(kt_extend(5, 0xFFFFFFFEu, true) == ((5ull << 32) | 0xFFFFFFFEu));
    /* simulated counter sweeping across the wrap with an ISR that runs late: monotonic */
    uint64_t prev = 0;
    uint32_t high = 7;
    for (uint64_t t = (7ull << 32) + 0xFFFFFF00u; t < (8ull << 32) + 0x200u; t++) {
        uint32_t low = (uint32_t)t;
        bool wrapped = (t >> 32) > high;
        bool isr_ran = wrapped && (low > 0x80u); /* ISR latency: runs 0x80 ticks after the wrap */
        if (isr_ran) {
            high = (uint32_t)(t >> 32);
        }
        uint64_t v = kt_extend(high, low, wrapped && !isr_ran);
        CHECK(v == t);
        CHECK(v >= prev);
        prev = v;
    }
    DONE();
    return 0;
}
