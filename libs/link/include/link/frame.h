/**
 * @file frame.h
 * @brief Frame = COBS([type][seq][payload][crc16]) + 0x00.
 */
#ifndef LINK_FRAME_H
#define LINK_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "link/cobs.h"
#include "link/protocol_defs.h"

/** Raw (un-stuffed) frame buffer size. The payload is built at raw + FRAME_PAYLOAD_OFFSET. */
#define FRAME_RAW_MAX (LINK_MAX_PAYLOAD + LINK_FRAME_OVERHEAD)
#define FRAME_PAYLOAD_OFFSET 2u
/** Wire buffer size: COBS worst case + delimiter. */
#define FRAME_WIRE_MAX (COBS_MAX_ENCODED(FRAME_RAW_MAX) + 1u)

typedef enum {
    FRAME_OK = 0,
    FRAME_ERR_COBS = 1,
    FRAME_ERR_SHORT = 2,
    FRAME_ERR_CRC = 3,
} frame_status_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    const uint8_t *payload;
    size_t payload_len;
} frame_t;

/**
 * Finish a frame whose payload already sits at raw[FRAME_PAYLOAD_OFFSET]:
 * writes the header and CRC into @p raw, then COBS-encodes into @p wire and
 * appends the 0x00 delimiter. Building the payload in place avoids a copy.
 * @return bytes to transmit, or 0 if it does not fit.
 */
size_t frame_finalize(uint8_t *raw, size_t payload_len, uint8_t type, uint8_t seq, uint8_t *wire,
                      size_t wire_cap);

/** Decode one received frame (delimiter already stripped) in place. */
frame_status_t frame_parse(uint8_t *buf, size_t len, frame_t *out);

#endif /* LINK_FRAME_H */
