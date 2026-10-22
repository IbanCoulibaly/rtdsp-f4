/**
 * @file cobs.c
 * @brief COBS encoder/decoder (Cheshire & Baker, 1999).
 */
#include "link/cobs.h"

size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    if (cap == 0u) {
        return 0u;
    }
    size_t code_idx = 0u; /* where the current block's code byte will go */
    size_t w = 1u;
    uint8_t code = 1u;

    for (size_t r = 0u; r < len; r++) {
        if (src[r] == 0u) {
            dst[code_idx] = code;
            code = 1u;
            code_idx = w++;
            if (code_idx >= cap) {
                return 0u;
            }
        } else {
            if (w >= cap) {
                return 0u;
            }
            dst[w++] = src[r];
            code++;
            if (code == 0xFFu) { /* 254 data bytes: close the block without an implied zero */
                dst[code_idx] = code;
                code = 1u;
                code_idx = w++;
                if (code_idx >= cap) {
                    return 0u;
                }
            }
        }
    }
    dst[code_idx] = code;
    return w;
}

int cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len)
{
    size_t r = 0u;
    size_t w = 0u;

    while (r < len) {
        const uint8_t code = src[r++];
        if (code == 0u) {
            return -1;
        }
        for (uint8_t i = 1u; i < code; i++) {
            if ((r >= len) || (src[r] == 0u) || (w >= cap)) {
                return -1;
            }
            dst[w++] = src[r++];
        }
        /* Every block except a full (0xFF) one and the last one implies a zero. */
        if ((code != 0xFFu) && (r < len)) {
            if (w >= cap) {
                return -1;
            }
            dst[w++] = 0u;
        }
    }
    *out_len = w;
    return 0;
}
