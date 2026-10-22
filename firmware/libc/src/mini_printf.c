/**
 * @file mini_printf.c
 * @brief Bounded vsnprintf subset. See mini_printf.h.
 */
#include "mini_printf.h"

#include <stdint.h>

typedef struct {
    char *buf;
    size_t size;
    size_t len; /* characters that would have been written */
} out_t;

static void put(out_t *o, char c)
{
    if (o->len + 1u < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void put_padded(out_t *o, const char *s, size_t n, int width, int left, char pad)
{
    int fill = width - (int)n;
    if (!left) {
        while (fill-- > 0) {
            put(o, pad);
        }
    }
    for (size_t i = 0u; i < n; i++) {
        put(o, s[i]);
    }
    if (left) {
        while (fill-- > 0) {
            put(o, ' ');
        }
    }
}

static void put_uint(out_t *o, uint32_t v, unsigned base, int upper, int neg, int width, int left, char pad)
{
    char tmp[12];
    size_t n = 0u;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0u);
    if (neg) {
        if (pad == '0') {
            put(o, '-'); /* sign goes before zero padding: -0042 */
            width--;
        } else {
            tmp[n++] = '-';
        }
    }
    char rev[12];
    for (size_t i = 0u; i < n; i++) {
        rev[i] = tmp[n - 1u - i];
    }
    put_padded(o, rev, n, width, left, pad);
}

int mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = {buf, size, 0u};

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p++;
        int left = 0;
        char pad = ' ';
        int width = 0;
        for (;; p++) {
            if (*p == '-') {
                left = 1;
            } else if (*p == '0') {
                pad = '0';
            } else {
                break;
            }
        }
        while (*p >= '0' && *p <= '9') {
            width = (width * 10) + (*p - '0');
            p++;
        }
        if (*p == 'l') {
            p++; /* long is 32-bit on this target */
        }
        switch (*p) {
        case 'd':
        case 'i': {
            const int32_t v = va_arg(ap, int32_t);
            const uint32_t mag = (v < 0) ? (uint32_t)0u - (uint32_t)v : (uint32_t)v;
            put_uint(&o, mag, 10u, 0, v < 0, width, left, pad);
            break;
        }
        case 'u':
            put_uint(&o, va_arg(ap, uint32_t), 10u, 0, 0, width, left, pad);
            break;
        case 'x':
        case 'X':
            put_uint(&o, va_arg(ap, uint32_t), 16u, *p == 'X', 0, width, left, pad);
            break;
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_uint(&o, (uint32_t)(uintptr_t)va_arg(ap, void *), 16u, 0, 0, 8, 0, '0');
            break;
        case 'c': {
            const char c = (char)va_arg(ap, int);
            put_padded(&o, &c, 1u, width, left, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t n = 0u;
            while (s[n] != '\0') {
                n++;
            }
            put_padded(&o, s, n, width, left, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case '\0':
            p--; /* trailing '%': stop at the terminator */
            break;
        default:
            put(&o, '%');
            put(&o, *p);
            break;
        }
    }
    if (size > 0u) {
        o.buf[(o.len < size) ? o.len : size - 1u] = '\0';
    }
    return (int)o.len;
}

int mini_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = mini_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
