/**
 * @file mini_libc.c
 * @brief The few C library routines the firmware needs.
 *
 * GCC may emit calls to memcpy/memset for struct copies even with
 * -ffreestanding, so these must exist. Compiled with
 * -fno-tree-loop-distribute-patterns so GCC cannot "optimise" the loops
 * below back into recursive calls to themselves.
 */
#include <stdint.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    /* Word copies when both pointers are aligned: 4x fewer bus cycles. */
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        while (n >= 4u) {
            *(uint32_t *)(void *)d = *(const uint32_t *)(const void *)s;
            d += 4;
            s += 4;
            n -= 4u;
        }
    }
    while (n-- > 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s) {
        while (n-- > 0u) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n-- > 0u) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0u) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    for (size_t i = 0u; i < n; i++) {
        if (p[i] != q[i]) {
            return (int)p[i] - (int)q[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}
