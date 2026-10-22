/**
 * @file fir.h
 * @brief Direct-form FIR filters (Q15 fixed-point and float32).
 *
 *     y[n] = sum_{k=0}^{N-1} h[k] * x[n-k]
 *
 * The delay line is a "mirrored" circular buffer of 2N samples: each new
 * sample is written at idx and idx+N, so the window x[n..n-N+1] is always
 * contiguous at state[idx .. idx+N-1]. The inner MAC loop therefore has no
 * modulo/wrap check, at the cost of one extra store per sample.
 *
 * Kernels are in-place safe (in == out is allowed).
 */
#ifndef DSP_FIR_H
#define DSP_FIR_H

#include "dsp/dsp_types.h"

typedef struct {
    const q15_t *coeffs; /**< h[0..N-1] in Q15. */
    q15_t *state;        /**< 2*N samples, caller-owned. */
    uint16_t num_taps;
    uint16_t idx;
} fir_q15_t;

typedef struct {
    const f32_t *coeffs;
    f32_t *state; /**< 2*N samples, caller-owned. */
    uint16_t num_taps;
    uint16_t idx;
} fir_f32_t;

void fir_q15_init(fir_q15_t *f, const q15_t *coeffs, uint16_t num_taps, q15_t *state);
void fir_q15_reset(fir_q15_t *f);
void fir_q15_process(fir_q15_t *f, const q15_t *in, q15_t *out, size_t n);

void fir_f32_init(fir_f32_t *f, const f32_t *coeffs, uint16_t num_taps, f32_t *state);
void fir_f32_reset(fir_f32_t *f);
void fir_f32_process(fir_f32_t *f, const f32_t *in, f32_t *out, size_t n);

#endif /* DSP_FIR_H */
