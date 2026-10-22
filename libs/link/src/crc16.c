/**
 * @file crc16.c
 * @brief Nibble-table CRC-16/CCITT-FALSE.
 *
 * A 16-entry table (32 bytes of flash) processes 4 bits per lookup: a good
 * trade-off between the bitwise loop (8 iterations/byte) and the classic
 * 256-entry table (512 bytes).
 */
#include "link/crc16.h"

static const uint16_t k_crc_nibble[16] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
};

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0u; i < len; i++) {
        const uint8_t b = data[i];
        crc = (uint16_t)((crc << 4) ^ k_crc_nibble[((crc >> 12) ^ (b >> 4)) & 0x0Fu]);
        crc = (uint16_t)((crc << 4) ^ k_crc_nibble[((crc >> 12) ^ (b & 0x0Fu)) & 0x0Fu]);
    }
    return crc;
}
