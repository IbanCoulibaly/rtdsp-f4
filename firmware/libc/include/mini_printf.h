/**
 * @file mini_printf.h
 * @brief Bounded, allocation-free vsnprintf subset.
 *
 * Supports: %d %i %u %x %X %c %s %p %% with optional '-', '0', width and
 * 'l'. No floating point (keeps the formatter out of the FPU context and
 * small); print fixed-point values as scaled integers instead.
 */
#ifndef MINI_PRINTF_H
#define MINI_PRINTF_H

#include <stdarg.h>
#include <stddef.h>

int mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int mini_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif /* MINI_PRINTF_H */
