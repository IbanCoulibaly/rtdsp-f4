/**
 * @file byteorder.h
 * @brief Explicit little-endian (de)serialisation.
 *
 * Wire structs are never cast onto buffers: that would depend on compiler
 * padding, host endianness and alignment (unaligned access faults on many
 * cores). Fields are written one by one instead.
 */
#ifndef LINK_BYTEORDER_H
#define LINK_BYTEORDER_H

#include <stdint.h>

static inline uint8_t *put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static inline uint8_t *put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* LINK_BYTEORDER_H */
