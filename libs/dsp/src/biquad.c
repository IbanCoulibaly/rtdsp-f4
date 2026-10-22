/**
 * @file biquad.c
 * @brief Biquad cascade kernels. See biquad.h and docs/DSP_MATH.md section 5.
 *
 * Both kernels are stage-major: the whole block runs through stage 0, then
 * stage 1, and so on. A stage's five coefficients and its state then live
 * in registers for the duration of the block instead of being reloaded
 * every sample.
 */
#include "dsp/biquad.h"

#include "dsp/fixed_point.h"

void biquad_f32_init(biquad_f32_t *bq, const f32_t *coeffs, uint8_t num_stages, f32_t *state)
{
    bq->coeffs = coeffs;
    bq->state = state;
    bq->num_stages = num_stages;
    biquad_f32_reset(bq);
}

void biquad_f32_reset(biquad_f32_t *bq)
{
    for (uint32_t i = 0u; i < 2u * (uint32_t)bq->num_stages; i++) {
        bq->state[i] = 0.0f;
    }
}

void biquad_f32_process(biquad_f32_t *bq, const f32_t *in, f32_t *out, size_t n)
{
    const f32_t *c = bq->coeffs;
    f32_t *s = bq->state;
    const f32_t *src = in;

    for (uint8_t stage = 0u; stage < bq->num_stages; stage++) {
        const f32_t b0 = c[0];
        const f32_t b1 = c[1];
        const f32_t b2 = c[2];
        const f32_t na1 = c[3];
        const f32_t na2 = c[4];
        f32_t s1 = s[0];
        f32_t s2 = s[1];

        /* Direct Form II Transposed:
         *   y  = b0*x + s1
         *   s1 = b1*x - a1*y + s2
         *   s2 = b2*x - a2*y            */
        for (size_t i = 0u; i < n; i++) {
            const f32_t x = src[i];
            const f32_t y = (b0 * x) + s1;
            s1 = (b1 * x) + (na1 * y) + s2;
            s2 = (b2 * x) + (na2 * y);
            out[i] = y;
        }

        s[0] = s1;
        s[1] = s2;
        c += BIQUAD_COEFFS_PER_STAGE;
        s += 2;
        src = out; /* later stages filter the previous stage's output in place */
    }
}

void biquad_q31_init(biquad_q31_t *bq, const q31_t *coeffs, uint8_t num_stages, uint8_t post_shift,
                     q31_t *state)
{
    bq->coeffs = coeffs;
    bq->state = state;
    bq->num_stages = num_stages;
    bq->post_shift = post_shift;
    biquad_q31_reset(bq);
}

void biquad_q31_reset(biquad_q31_t *bq)
{
    for (uint32_t i = 0u; i < 4u * (uint32_t)bq->num_stages; i++) {
        bq->state[i] = 0;
    }
}

void biquad_q31_process(biquad_q31_t *bq, const q31_t *in, q31_t *out, size_t n)
{
    const q31_t *c = bq->coeffs;
    q31_t *s = bq->state;
    const q31_t *src = in;
    /* Q31 data * Q(31-ps) coefficients = Q(62-ps) products; >> (31-ps) gives Q31. */
    const uint32_t shift = 31u - (uint32_t)bq->post_shift;
    const int64_t round = (int64_t)1 << (shift - 1u);

    for (uint8_t stage = 0u; stage < bq->num_stages; stage++) {
        const int64_t b0 = c[0];
        const int64_t b1 = c[1];
        const int64_t b2 = c[2];
        const int64_t na1 = c[3];
        const int64_t na2 = c[4];
        q31_t x1 = s[0];
        q31_t x2 = s[1];
        q31_t y1 = s[2];
        q31_t y2 = s[3];

        /* Direct Form I:
         *   y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]
         * The quantizer (python/rtdsp/quantize.py) picks post_shift so that
         * sum|c| < 2^(1+ps), which bounds |acc| < 2^63: no accumulator overflow
         * is possible for any input, so only the final store saturates. */
        for (size_t i = 0u; i < n; i++) {
            const q31_t x = src[i];
            int64_t acc = (b0 * x) + (b1 * x1) + (b2 * x2) + (na1 * y1) + (na2 * y2);
            const q31_t y = dsp_sat_q31((acc + round) >> shift);
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            out[i] = y;
        }

        s[0] = x1;
        s[1] = x2;
        s[2] = y1;
        s[3] = y2;
        c += BIQUAD_COEFFS_PER_STAGE;
        s += 4;
        src = out;
    }
}
