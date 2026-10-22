/**
 * @file biquad.h
 * @brief Cascaded second-order IIR sections (biquads).
 *
 * Each section implements
 *
 *            b0 + b1 z^-1 + b2 z^-2
 *   H_k(z) = ----------------------
 *            1  + a1 z^-1 + a2 z^-2
 *
 * Coefficients are stored per stage as {b0, b1, b2, -a1, -a2}: the feedback
 * terms are pre-negated so every term of the difference equation is a
 * multiply-accumulate (same layout as CMSIS-DSP).
 *
 *  - biquad_f32: Direct Form II Transposed, 2 states/stage, float32 (FPU).
 *  - biquad_q31: Direct Form I, 4 states/stage, Q31 data, 64-bit accumulator.
 *    DF1 is preferred in fixed point because its only internal node is the
 *    accumulator itself, so it cannot overflow between the two halves of
 *    the section the way the DF2 internal state can.
 *
 * Kernels are in-place safe (in == out is allowed).
 */
#ifndef DSP_BIQUAD_H
#define DSP_BIQUAD_H

#include "dsp/dsp_types.h"

#define BIQUAD_COEFFS_PER_STAGE 5u

typedef struct {
    const f32_t *coeffs; /**< 5 * num_stages. */
    f32_t *state;        /**< 2 * num_stages: {s1, s2}. */
    uint8_t num_stages;
} biquad_f32_t;

typedef struct {
    const q31_t *coeffs; /**< 5 * num_stages, scaled by 2^-post_shift. */
    q31_t *state;        /**< 4 * num_stages: {x[n-1], x[n-2], y[n-1], y[n-2]}. */
    uint8_t num_stages;
    uint8_t post_shift; /**< Coefficients are Q(31 - post_shift). */
} biquad_q31_t;

void biquad_f32_init(biquad_f32_t *bq, const f32_t *coeffs, uint8_t num_stages, f32_t *state);
void biquad_f32_reset(biquad_f32_t *bq);
void biquad_f32_process(biquad_f32_t *bq, const f32_t *in, f32_t *out, size_t n);

void biquad_q31_init(biquad_q31_t *bq, const q31_t *coeffs, uint8_t num_stages, uint8_t post_shift,
                     q31_t *state);
void biquad_q31_reset(biquad_q31_t *bq);
void biquad_q31_process(biquad_q31_t *bq, const q31_t *in, q31_t *out, size_t n);

#endif /* DSP_BIQUAD_H */
