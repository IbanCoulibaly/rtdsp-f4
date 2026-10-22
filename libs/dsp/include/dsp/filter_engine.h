/**
 * @file filter_engine.h
 * @brief Runtime-configurable filter: one "coefficient bank" + its state.
 *
 * The engine is the unit the firmware swaps atomically between blocks. It
 * owns a private copy of the coefficients, the delay-line state and the
 * scratch buffers needed to convert the Q15 I/O format (what an ADC/DAC
 * delivers) to the kernel's internal format (Q15, Q31 or float32).
 *
 * The same code is compiled for the Cortex-M4 target and for the host,
 * where Python drives it through ctypes (software-in-the-loop tests).
 */
#ifndef DSP_FILTER_ENGINE_H
#define DSP_FILTER_ENGINE_H

#include "dsp/biquad.h"
#include "dsp/dsp_types.h"
#include "dsp/fir.h"

#ifndef FILTER_MAX_TAPS
#define FILTER_MAX_TAPS 128u
#endif
#ifndef FILTER_MAX_STAGES
#define FILTER_MAX_STAGES 8u
#endif
#ifndef FILTER_BLOCK_MAX
#define FILTER_BLOCK_MAX 64u /**< Internal chunk size; any n is accepted by process(). */
#endif
#define FILTER_MAX_POST_SHIFT 7u

/** Wire header: type u8, post_shift u8, order u16 (LE), then coefficients. */
#define FILTER_DESC_HEADER_LEN 4u

typedef enum {
    FILTER_BYPASS = 0,
    FILTER_FIR_Q15 = 1,
    FILTER_FIR_F32 = 2,
    FILTER_BIQUAD_Q31 = 3,
    FILTER_BIQUAD_F32 = 4,
    FILTER_TYPE_COUNT
} filter_type_t;

typedef enum {
    FILTER_OK = 0,
    FILTER_ERR_TYPE = 1,     /**< Unknown filter type. */
    FILTER_ERR_ORDER = 2,    /**< Taps/stages out of range. */
    FILTER_ERR_LEN = 3,      /**< Payload length does not match order. */
    FILTER_ERR_PARAM = 4,    /**< Bad post_shift or non-finite coefficient. */
    FILTER_ERR_UNSTABLE = 5, /**< A biquad has a pole on/outside the unit circle. */
} filter_status_t;

/** Coefficient bank. */
typedef struct {
    uint8_t type;       /**< filter_type_t */
    uint8_t post_shift; /**< Q31 biquad only: coefficients are Q(31 - post_shift). */
    uint16_t order;     /**< FIR: taps. Biquad: stages. Bypass: 0. */
    union {
        q15_t fir_q15[FILTER_MAX_TAPS];
        f32_t fir_f32[FILTER_MAX_TAPS];
        q31_t bq_q31[BIQUAD_COEFFS_PER_STAGE * FILTER_MAX_STAGES];
        f32_t bq_f32[BIQUAD_COEFFS_PER_STAGE * FILTER_MAX_STAGES];
    } c;
} filter_desc_t;

typedef struct {
    filter_desc_t desc;
    union {
        q15_t fir_q15[2u * FILTER_MAX_TAPS];
        f32_t fir_f32[2u * FILTER_MAX_TAPS];
        q31_t bq_q31[4u * FILTER_MAX_STAGES];
        f32_t bq_f32[2u * FILTER_MAX_STAGES];
    } state;
    union {
        fir_q15_t fir_q15;
        fir_f32_t fir_f32;
        biquad_q31_t bq_q31;
        biquad_f32_t bq_f32;
    } k;
    union {
        q31_t q31[FILTER_BLOCK_MAX];
        f32_t f32[FILTER_BLOCK_MAX];
    } scratch;
} filter_engine_t;

/** Decode a little-endian wire descriptor (see FILTER_DESC_HEADER_LEN) and validate it. */
filter_status_t filter_desc_parse(filter_desc_t *d, const uint8_t *buf, size_t len);

/** Range, finiteness and stability checks (stability triangle per biquad). */
filter_status_t filter_desc_validate(const filter_desc_t *d);

/** Copy + validate @p d into the engine and clear its state. */
filter_status_t filter_engine_configure(filter_engine_t *e, const filter_desc_t *d);

/** Clear the delay lines; coefficients are kept. */
void filter_engine_reset(filter_engine_t *e);

/** Filter n Q15 samples. in == out is allowed. */
void filter_engine_process(filter_engine_t *e, const q15_t *in, q15_t *out, size_t n);

#endif /* DSP_FILTER_ENGINE_H */
