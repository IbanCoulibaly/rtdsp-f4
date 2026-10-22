/**
 * @file frame.c
 * @brief Framing: header + CRC + COBS.
 */
#include "link/frame.h"

#include "link/byteorder.h"
#include "link/crc16.h"

size_t frame_finalize(uint8_t *raw, size_t payload_len, uint8_t type, uint8_t seq, uint8_t *wire,
                      size_t wire_cap)
{
    if ((payload_len > (size_t)LINK_MAX_PAYLOAD) || (wire_cap < 2u)) {
        return 0u;
    }
    raw[0] = type;
    raw[1] = seq;
    const size_t body = FRAME_PAYLOAD_OFFSET + payload_len;
    (void)put_le16(&raw[body], crc16_ccitt(raw, body, CRC16_INIT));

    const size_t n = cobs_encode(raw, body + 2u, wire, wire_cap - 1u);
    if (n == 0u) {
        return 0u;
    }
    wire[n] = 0u;
    return n + 1u;
}

frame_status_t frame_parse(uint8_t *buf, size_t len, frame_t *out)
{
    size_t n = 0u;
    if (cobs_decode(buf, len, buf, len, &n) != 0) {
        return FRAME_ERR_COBS;
    }
    if (n < (size_t)LINK_FRAME_OVERHEAD) {
        return FRAME_ERR_SHORT;
    }
    const size_t body = n - 2u;
    if (crc16_ccitt(buf, body, CRC16_INIT) != get_le16(&buf[body])) {
        return FRAME_ERR_CRC;
    }
    out->type = buf[0];
    out->seq = buf[1];
    out->payload = &buf[FRAME_PAYLOAD_OFFSET];
    out->payload_len = body - FRAME_PAYLOAD_OFFSET;
    return FRAME_OK;
}
