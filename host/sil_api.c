/**
 * @file sil_api.c
 * @brief Flat C ABI over the portable libraries for Python ctypes.
 *
 * Software-in-the-loop: python/tests/test_sil.py loads this shared library
 * and runs the *same* filter_engine/link code the firmware runs, comparing
 * it against bit-exact Python reference models and scipy.
 */
#include <stddef.h>
#include <stdint.h>

#include "dsp/filter_engine.h"
#include "link/cobs.h"
#include "link/crc16.h"
#include "link/frame.h"

#if defined(_WIN32)
#define SIL_API __declspec(dllexport)
#else
#define SIL_API __attribute__((visibility("default")))
#endif

SIL_API size_t sil_engine_size(void)
{
    return sizeof(filter_engine_t);
}

SIL_API int sil_engine_configure(void *engine, const uint8_t *desc, size_t len)
{
    filter_desc_t d;
    const filter_status_t st = filter_desc_parse(&d, desc, len);
    if (st != FILTER_OK) {
        return (int)st;
    }
    return (int)filter_engine_configure((filter_engine_t *)engine, &d);
}

SIL_API void sil_engine_reset(void *engine)
{
    filter_engine_reset((filter_engine_t *)engine);
}

SIL_API void sil_engine_process(void *engine, const int16_t *in, int16_t *out, size_t n)
{
    filter_engine_process((filter_engine_t *)engine, in, out, n);
}

SIL_API uint16_t sil_crc16(const uint8_t *data, size_t len)
{
    return crc16_ccitt(data, len, CRC16_INIT);
}

SIL_API size_t sil_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    return cobs_encode(src, len, dst, cap);
}

SIL_API int sil_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len)
{
    return cobs_decode(src, len, dst, cap, out_len);
}

SIL_API size_t sil_frame_build(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *wire,
                               size_t cap)
{
    static uint8_t raw[FRAME_RAW_MAX];
    if (len > (size_t)LINK_MAX_PAYLOAD) {
        return 0u;
    }
    for (size_t i = 0u; i < len; i++) {
        raw[FRAME_PAYLOAD_OFFSET + i] = payload[i];
    }
    return frame_finalize(raw, len, type, seq, wire, cap);
}
