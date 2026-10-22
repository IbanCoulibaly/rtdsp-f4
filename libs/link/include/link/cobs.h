/**
 * @file cobs.h
 * @brief Consistent Overhead Byte Stuffing.
 *
 * COBS removes every 0x00 from a packet so 0x00 can delimit frames on a raw
 * byte stream. Overhead is at most ceil(n / 254) bytes, and a receiver that
 * joins mid-stream resynchronises at the next 0x00.
 */
#ifndef LINK_COBS_H
#define LINK_COBS_H

#include <stddef.h>
#include <stdint.h>

/** Worst-case encoded size of n bytes (excluding the 0x00 delimiter). */
#define COBS_MAX_ENCODED(n) ((n) + ((n) / 254u) + 1u)

/** @return encoded length, or 0 if @p cap is too small. */
size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

/** In-place safe (dst == src). @return 0 on success, -1 on malformed input or overflow. */
int cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len);

#endif /* LINK_COBS_H */
