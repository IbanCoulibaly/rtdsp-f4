/**
 * @file post.c
 * @brief Power-on self test.
 *
 * Runs a deterministic pseudo-random input through the production filter
 * engine and compares a CRC of the output with golden values computed by
 * the bit-exact Python model (python/tools/gen_firmware_tables.py). A pass
 * proves that the compiler, the FPU/DSP instructions and the kernels on
 * *this* target agree with the reference design.
 */
#include <string.h>

#include "app.h"

#include "app_services.h"
#include "bsp/bsp.h"
#include "bsp/bsp_timer.h"
#include "fw_tables.h"
#include "link/crc16.h"
#include "link/protocol_defs.h"

static q15_t s_in[POST_INPUT_LEN];
static q15_t s_out[POST_INPUT_LEN];

static void make_input(void)
{
    uint32_t x = POST_INPUT_SEED;
    for (uint32_t i = 0u; i < POST_INPUT_LEN; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_in[i] = (q15_t)(int16_t)(x >> 16);
    }
}

static uint16_t run_and_crc(filter_engine_t *e, const filter_desc_t *d)
{
    if (filter_engine_configure(e, d) != FILTER_OK) {
        return 0u;
    }
    /* Uneven chunks exercise state carry-over across block boundaries. */
    uint32_t pos = 0u;
    uint32_t chunk = 1u;
    while (pos < POST_INPUT_LEN) {
        const uint32_t n = (pos + chunk > POST_INPUT_LEN) ? POST_INPUT_LEN - pos : chunk;
        filter_engine_process(e, &s_in[pos], &s_out[pos], n);
        pos += n;
        chunk = (chunk % 37u) + 7u;
    }
    uint16_t crc = CRC16_INIT;
    for (uint32_t i = 0u; i < POST_INPUT_LEN; i++) {
        const uint8_t le[2] = {(uint8_t)s_out[i], (uint8_t)((uint16_t)s_out[i] >> 8)};
        crc = crc16_ccitt(le, 2u, crc);
    }
    return crc;
}

uint8_t post_run(filter_engine_t *scratch)
{
    uint8_t result = 0u;

    static const uint8_t k_check[] = "123456789";
    if (crc16_ccitt(k_check, 9u, CRC16_INIT) != CRC16_CHECK) {
        result |= POST_FAIL_CRC_ENGINE;
    }

    make_input();
    if (run_and_crc(scratch, &k_default_filter) != POST_FIR_Q15_CRC) {
        result |= POST_FAIL_FIR_Q15;
    }
    if (run_and_crc(scratch, &k_post_biquad_q31) != POST_BIQUAD_Q31_CRC) {
        result |= POST_FAIL_BIQUAD_Q31;
    }

    /* Float FIR: an impulse of exactly 1.0 must reproduce h[] bit for bit
     * (1.0 * h + 0.0 * ... is exact in IEEE-754). */
    static f32_t h[16], st[32], x[24], y[24];
    for (uint32_t i = 0u; i < 16u; i++) {
        h[i] = (f32_t)k_default_filter.c.fir_q15[i] * (1.0f / 32768.0f);
    }
    memset(x, 0, sizeof x);
    x[0] = 1.0f;
    fir_f32_t f;
    fir_f32_init(&f, h, 16u, st);
    fir_f32_process(&f, x, y, 24u);
    for (uint32_t i = 0u; i < 24u; i++) {
        const f32_t expect = (i < 16u) ? h[i] : 0.0f;
        if (y[i] != expect) {
            result |= POST_FAIL_FIR_F32;
        }
    }

#if !BSP_TARGET_QEMU
    /* Clock tree: the timer clock measured against the core clock must be
     * the nominal APB1x2 rate. A failed PLL lock or a wrong prescaler shows
     * up here instead of as a wrong sample rate in the field. */
    const uint32_t tim = bsp_timer_clock_hz();
    const uint32_t tol = BSP_TIM_CLOCK_HZ / 50u;
    if ((tim < BSP_TIM_CLOCK_HZ - tol) || (tim > BSP_TIM_CLOCK_HZ + tol)) {
        result |= POST_FAIL_CLOCK;
    }
#endif

    filter_engine_reset(scratch);
    return result;
}
