/**
 * @file string.h
 * @brief Freestanding subset of <string.h>.
 *
 * The firmware links with -nostdlib: no newlib, no hidden malloc, no
 * reentrancy hooks. FreeRTOS and the libraries need only these four.
 */
#ifndef RTDSP_STRING_H
#define RTDSP_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif /* RTDSP_STRING_H */
