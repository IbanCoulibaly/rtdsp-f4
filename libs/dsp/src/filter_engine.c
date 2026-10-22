/**
 * @file filter_engine.c
 * @brief Descriptor parsing/validation and Q15 <-> kernel-format glue.
 */
#include "dsp/filter_engine.h"

#include <string.h>

#include "dsp/fixed_point.h"

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static f32_t rd_lef32(const uint8_t *p)
{
    const uint32_t u = rd_le32(p);
    f32_t f;
    memcpy(&f, &u, sizeof f); /* type-pun without violating strict aliasing */
    return f;
}

static int f32_is_finite(f32_t x)
{
    /* NaN fails x == x; +/-Inf and absurd magnitudes fail the bound. No libm needed. */
    return (x == x) && (x < 1.0e6f) && (x > -1.0e6f);
}

static size_t coeff_count(const filter_desc_t *d)
{
    switch (d->type) {
    case FILTER_FIR_Q15:
    case FILTER_FIR_F32:
        return d->order;
    case FILTER_BIQUAD_Q31:
    case FILTER_BIQUAD_F32:
        return (size_t)d->order * BIQUAD_COEFFS_PER_STAGE;
    default:
        return 0u;
    }
}

static size_t coeff_size(uint8_t type)
{
    return (type == FILTER_FIR_Q15) ? 2u : 4u;
}

static int order_in_range(const filter_desc_t *d)
{
    switch (d->type) {
    case FILTER_BYPASS:
        return d->order == 0u;
    case FILTER_FIR_Q15:
    case FILTER_FIR_F32:
        return (d->order >= 1u) && (d->order <= FILTER_MAX_TAPS);
    case FILTER_BIQUAD_Q31:
    case FILTER_BIQUAD_F32:
        return (d->order >= 1u) && (d->order <= FILTER_MAX_STAGES);
    default:
        return 0;
    }
}

filter_status_t filter_desc_parse(filter_desc_t *d, const uint8_t *buf, size_t len)
{
    if (len < FILTER_DESC_HEADER_LEN) {
        return FILTER_ERR_LEN;
    }
    memset(d, 0, sizeof *d);
    d->type = buf[0];
    d->post_shift = buf[1];
    d->order = rd_le16(&buf[2]);

    if (d->type >= (uint8_t)FILTER_TYPE_COUNT) {
        return FILTER_ERR_TYPE;
    }
    /* Range-check order before it is used to size anything. */
    if (!order_in_range(d)) {
        return FILTER_ERR_ORDER;
    }

    const size_t count = coeff_count(d);
    const size_t sz = coeff_size(d->type);
    if (len != FILTER_DESC_HEADER_LEN + count * sz) {
        return FILTER_ERR_LEN;
    }

    const uint8_t *p = &buf[FILTER_DESC_HEADER_LEN];
    for (size_t i = 0u; i < count; i++, p += sz) {
        switch (d->type) {
        case FILTER_FIR_Q15:
            d->c.fir_q15[i] = (q15_t)rd_le16(p);
            break;
        case FILTER_FIR_F32:
            d->c.fir_f32[i] = rd_lef32(p);
            break;
        case FILTER_BIQUAD_Q31:
            d->c.bq_q31[i] = (q31_t)rd_le32(p);
            break;
        case FILTER_BIQUAD_F32:
            d->c.bq_f32[i] = rd_lef32(p);
            break;
        default:
            break;
        }
    }
    return filter_desc_validate(d);
}

/* Both poles of 1 + a1 z^-1 + a2 z^-2 lie strictly inside |z| = 1 iff
 * |a2| < 1 and |a1| < 1 + a2 (the "stability triangle", Jury criterion).
 * Coefficients are stored negated, so a1 = -c[3], a2 = -c[4]. */
static int biquad_f32_stable(const f32_t *c)
{
    const f32_t a1 = -c[3];
    const f32_t a2 = -c[4];
    const f32_t abs_a1 = (a1 < 0.0f) ? -a1 : a1;
    const f32_t abs_a2 = (a2 < 0.0f) ? -a2 : a2;
    return (abs_a2 < 1.0f) && (abs_a1 < (1.0f + a2));
}

static int biquad_q31_stable(const q31_t *c, uint8_t post_shift)
{
    /* Same test in integers: "1.0" is 2^(31 - post_shift). */
    const int64_t one = (int64_t)1 << (31u - post_shift);
    const int64_t a1 = -(int64_t)c[3];
    const int64_t a2 = -(int64_t)c[4];
    const int64_t abs_a1 = (a1 < 0) ? -a1 : a1;
    const int64_t abs_a2 = (a2 < 0) ? -a2 : a2;
    return (abs_a2 < one) && (abs_a1 < (one + a2));
}

filter_status_t filter_desc_validate(const filter_desc_t *d)
{
    switch (d->type) {
    case FILTER_BYPASS:
        if (d->order != 0u) {
            return FILTER_ERR_ORDER;
        }
        return (d->post_shift == 0u) ? FILTER_OK : FILTER_ERR_PARAM;

    case FILTER_FIR_Q15:
    case FILTER_FIR_F32:
        if ((d->order == 0u) || (d->order > FILTER_MAX_TAPS)) {
            return FILTER_ERR_ORDER;
        }
        if (d->post_shift != 0u) {
            return FILTER_ERR_PARAM;
        }
        if (d->type == FILTER_FIR_F32) {
            for (uint16_t i = 0u; i < d->order; i++) {
                if (!f32_is_finite(d->c.fir_f32[i])) {
                    return FILTER_ERR_PARAM;
                }
            }
        }
        return FILTER_OK;

    case FILTER_BIQUAD_Q31:
    case FILTER_BIQUAD_F32:
        if ((d->order == 0u) || (d->order > FILTER_MAX_STAGES)) {
            return FILTER_ERR_ORDER;
        }
        if (d->type == FILTER_BIQUAD_Q31) {
            if (d->post_shift > FILTER_MAX_POST_SHIFT) {
                return FILTER_ERR_PARAM;
            }
            for (uint16_t s = 0u; s < d->order; s++) {
                if (!biquad_q31_stable(&d->c.bq_q31[s * BIQUAD_COEFFS_PER_STAGE], d->post_shift)) {
                    return FILTER_ERR_UNSTABLE;
                }
            }
        } else {
            if (d->post_shift != 0u) {
                return FILTER_ERR_PARAM;
            }
            for (uint16_t i = 0u; i < d->order * BIQUAD_COEFFS_PER_STAGE; i++) {
                if (!f32_is_finite(d->c.bq_f32[i])) {
                    return FILTER_ERR_PARAM;
                }
            }
            for (uint16_t s = 0u; s < d->order; s++) {
                if (!biquad_f32_stable(&d->c.bq_f32[s * BIQUAD_COEFFS_PER_STAGE])) {
                    return FILTER_ERR_UNSTABLE;
                }
            }
        }
        return FILTER_OK;

    default:
        return FILTER_ERR_TYPE;
    }
}

filter_status_t filter_engine_configure(filter_engine_t *e, const filter_desc_t *d)
{
    const filter_status_t st = filter_desc_validate(d);
    if (st != FILTER_OK) {
        return st;
    }
    if (&e->desc != d) {
        e->desc = *d;
    }

    const filter_desc_t *c = &e->desc;
    switch (c->type) {
    case FILTER_FIR_Q15:
        fir_q15_init(&e->k.fir_q15, c->c.fir_q15, c->order, e->state.fir_q15);
        break;
    case FILTER_FIR_F32:
        fir_f32_init(&e->k.fir_f32, c->c.fir_f32, c->order, e->state.fir_f32);
        break;
    case FILTER_BIQUAD_Q31:
        biquad_q31_init(&e->k.bq_q31, c->c.bq_q31, (uint8_t)c->order, c->post_shift, e->state.bq_q31);
        break;
    case FILTER_BIQUAD_F32:
        biquad_f32_init(&e->k.bq_f32, c->c.bq_f32, (uint8_t)c->order, e->state.bq_f32);
        break;
    default:
        break;
    }
    return FILTER_OK;
}

void filter_engine_reset(filter_engine_t *e)
{
    switch (e->desc.type) {
    case FILTER_FIR_Q15:
        fir_q15_reset(&e->k.fir_q15);
        break;
    case FILTER_FIR_F32:
        fir_f32_reset(&e->k.fir_f32);
        break;
    case FILTER_BIQUAD_Q31:
        biquad_q31_reset(&e->k.bq_q31);
        break;
    case FILTER_BIQUAD_F32:
        biquad_f32_reset(&e->k.bq_f32);
        break;
    default:
        break;
    }
}

static void process_chunk(filter_engine_t *e, const q15_t *in, q15_t *out, size_t n)
{
    switch (e->desc.type) {
    case FILTER_FIR_Q15:
        fir_q15_process(&e->k.fir_q15, in, out, n);
        break;

    case FILTER_FIR_F32:
        for (size_t i = 0u; i < n; i++) {
            e->scratch.f32[i] = dsp_q15_to_f32(in[i]);
        }
        fir_f32_process(&e->k.fir_f32, e->scratch.f32, e->scratch.f32, n);
        for (size_t i = 0u; i < n; i++) {
            out[i] = dsp_f32_to_q15(e->scratch.f32[i]);
        }
        break;

    case FILTER_BIQUAD_Q31:
        for (size_t i = 0u; i < n; i++) {
            e->scratch.q31[i] = dsp_q15_to_q31(in[i]);
        }
        biquad_q31_process(&e->k.bq_q31, e->scratch.q31, e->scratch.q31, n);
        for (size_t i = 0u; i < n; i++) {
            out[i] = dsp_q31_to_q15(e->scratch.q31[i]);
        }
        break;

    case FILTER_BIQUAD_F32:
        for (size_t i = 0u; i < n; i++) {
            e->scratch.f32[i] = dsp_q15_to_f32(in[i]);
        }
        biquad_f32_process(&e->k.bq_f32, e->scratch.f32, e->scratch.f32, n);
        for (size_t i = 0u; i < n; i++) {
            out[i] = dsp_f32_to_q15(e->scratch.f32[i]);
        }
        break;

    case FILTER_BYPASS:
    default:
        if (in != out) {
            memmove(out, in, n * sizeof *out);
        }
        break;
    }
}

void filter_engine_process(filter_engine_t *e, const q15_t *in, q15_t *out, size_t n)
{
    while (n > 0u) {
        const size_t chunk = (n > FILTER_BLOCK_MAX) ? FILTER_BLOCK_MAX : n;
        process_chunk(e, in, out, chunk);
        in += chunk;
        out += chunk;
        n -= chunk;
    }
}
