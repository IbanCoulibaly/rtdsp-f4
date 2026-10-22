/**
 * @file crc16.h
 * @brief CRC-16/CCITT-FALSE: poly x^16 + x^12 + x^5 + 1 (0x1021), init 0xFFFF,
 *        no reflection, no final XOR. Check value for "123456789" is 0x29B1.
 */
#ifndef LINK_CRC16_H
#define LINK_CRC16_H

#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFFu
#define CRC16_CHECK 0x29B1u

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc);

#endif /* LINK_CRC16_H */
