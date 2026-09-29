#ifndef KTEST_UTIL_H
#define KTEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>

static int g_checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define DONE() printf("   %d checks ok\n", g_checks)

/* Deterministic PRNG (xorshift32) so fuzz runs are reproducible from the seed. */
static inline unsigned xs32(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
#endif
