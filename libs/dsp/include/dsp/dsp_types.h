/**
 * @file dsp_types.h
 * @brief Fixed-width sample types shared by every DSP kernel.
 *
 * Qm.n naming follows the ARM/CMSIS convention: a qN_t holds a signed
 * fraction in [-1, 1) with N fractional bits (value = integer / 2^N).
 */
#ifndef DSP_TYPES_H
#define DSP_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int16_t q15_t; /**< Q1.15: 1 sign bit, 15 fractional bits. */
typedef int32_t q31_t; /**< Q1.31: 1 sign bit, 31 fractional bits. */
typedef int64_t q63_t; /**< Wide accumulator (guard bits for MAC chains). */
typedef float f32_t;   /**< IEEE-754 binary32, maps to the Cortex-M4 FPU. */

#define DSP_Q15_MAX ((q15_t)0x7FFF)
#define DSP_Q15_MIN ((q15_t)-0x8000)
#define DSP_Q31_MAX ((q31_t)0x7FFFFFFF)
#define DSP_Q31_MIN ((q31_t)(-0x7FFFFFFF - 1))

#endif /* DSP_TYPES_H */
