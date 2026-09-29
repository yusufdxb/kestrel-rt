/* Freestanding memory primitives. GCC may emit calls to memcpy/memset/memmove for struct copies
 * and zeroing even with -ffreestanding, so the runtime provides them. No libc is linked. */
#include "kernel/memory/kstring.h"

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- != 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n-- != 0u) {
            *d++ = *s++;
        }
    } else {
        while (n-- != 0u) {
            d[n] = s[n];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n-- != 0u) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}
