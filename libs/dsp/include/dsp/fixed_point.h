/**
 * @file fixed_point.h
 * @brief Saturating fixed-point helpers and Q-format conversions.
 *
 * All conversions round half away from zero (float) or half up (integer)
 * and saturate instead of wrapping. Wrap-around in a filter feedback loop
 * produces full-scale limit cycles; saturation degrades gracefully.
 * See docs/DSP_MATH.md section 3.
 */
#ifndef DSP_FIXED_POINT_H
#define DSP_FIXED_POINT_H

#include "dsp/dsp_types.h"

static inline q15_t dsp_sat_q15(int64_t v)
{
    if (v > (int64_t)DSP_Q15_MAX) {
        return DSP_Q15_MAX;
    }
    if (v < (int64_t)DSP_Q15_MIN) {
        return DSP_Q15_MIN;
    }
    return (q15_t)v;
}

static inline q31_t dsp_sat_q31(int64_t v)
{
    if (v > (int64_t)DSP_Q31_MAX) {
        return DSP_Q31_MAX;
    }
    if (v < (int64_t)DSP_Q31_MIN) {
        return DSP_Q31_MIN;
    }
    return (q31_t)v;
}

/** Q15 -> Q31 is exact: multiply by 2^16 (done as a multiply to avoid UB on negative shifts). */
static inline q31_t dsp_q15_to_q31(q15_t x)
{
    return (q31_t)x * 65536;
}

/** Q31 -> Q15 with round-half-up and saturation. */
static inline q15_t dsp_q31_to_q15(q31_t x)
{
    return dsp_sat_q15(((int64_t)x + 0x8000) >> 16);
}

static inline f32_t dsp_q15_to_f32(q15_t x)
{
    return (f32_t)x * (1.0f / 32768.0f);
}

/** float -> Q15: scale, clamp in float domain (out-of-range float->int casts are UB), round. */
static inline q15_t dsp_f32_to_q15(f32_t x)
{
    const f32_t s = x * 32768.0f;
    if (s >= 32767.0f) {
        return DSP_Q15_MAX;
    }
    if (s <= -32768.0f) {
        return DSP_Q15_MIN;
    }
    return (q15_t)((s >= 0.0f) ? (s + 0.5f) : (s - 0.5f));
}

#endif /* DSP_FIXED_POINT_H */
