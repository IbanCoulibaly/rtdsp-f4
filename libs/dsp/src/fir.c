/**
 * @file fir.c
 * @brief FIR kernels. See fir.h and docs/DSP_MATH.md section 4.
 */
#include "dsp/fir.h"

#include "dsp/fixed_point.h"

void fir_q15_init(fir_q15_t *f, const q15_t *coeffs, uint16_t num_taps, q15_t *state)
{
    f->coeffs = coeffs;
    f->state = state;
    f->num_taps = num_taps;
    fir_q15_reset(f);
}

void fir_q15_reset(fir_q15_t *f)
{
    for (uint32_t i = 0u; i < 2u * (uint32_t)f->num_taps; i++) {
        f->state[i] = 0;
    }
    f->idx = 0u;
}

void fir_q15_process(fir_q15_t *f, const q15_t *in, q15_t *out, size_t n)
{
    const uint16_t taps = f->num_taps;
    const q15_t *h = f->coeffs;
    q15_t *st = f->state;
    uint16_t idx = f->idx;

    for (size_t i = 0u; i < n; i++) {
        /* Newest sample goes to the lowest index, so state[idx + k] == x[n - k]. */
        idx = (idx == 0u) ? (uint16_t)(taps - 1u) : (uint16_t)(idx - 1u);
        const q15_t x = in[i];
        st[idx] = x;
        st[idx + taps] = x;

        const q15_t *w = &st[idx];
        /* Q15 * Q15 = Q30 products. A 64-bit accumulator gives 33 guard bits,
         * far more than the log2(N) <= 7 needed for N <= 128 taps. */
        int64_t acc = 0;
        for (uint16_t k = 0u; k < taps; k++) {
            acc += (int32_t)h[k] * (int32_t)w[k];
        }
        out[i] = dsp_sat_q15((acc + (1 << 14)) >> 15);
    }
    f->idx = idx;
}

void fir_f32_init(fir_f32_t *f, const f32_t *coeffs, uint16_t num_taps, f32_t *state)
{
    f->coeffs = coeffs;
    f->state = state;
    f->num_taps = num_taps;
    fir_f32_reset(f);
}

void fir_f32_reset(fir_f32_t *f)
{
    for (uint32_t i = 0u; i < 2u * (uint32_t)f->num_taps; i++) {
        f->state[i] = 0.0f;
    }
    f->idx = 0u;
}

void fir_f32_process(fir_f32_t *f, const f32_t *in, f32_t *out, size_t n)
{
    const uint16_t taps = f->num_taps;
    const f32_t *h = f->coeffs;
    f32_t *st = f->state;
    uint16_t idx = f->idx;

    for (size_t i = 0u; i < n; i++) {
        idx = (idx == 0u) ? (uint16_t)(taps - 1u) : (uint16_t)(idx - 1u);
        const f32_t x = in[i];
        st[idx] = x;
        st[idx + taps] = x;

        const f32_t *w = &st[idx];
        f32_t acc = 0.0f;
        for (uint16_t k = 0u; k < taps; k++) {
            acc += h[k] * w[k];
        }
        out[i] = acc;
    }
    f->idx = idx;
}
