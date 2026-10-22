/**
 * @file siggen.c
 * @brief On-board test signal: two NCO tones + white noise, one sample per
 *        TIM2 interrupt. Stands in for "ADC + DMA" on the emulator.
 *
 * NCO (numerically controlled oscillator), docs/DSP_MATH.md section 7:
 *   phase[n+1] = phase[n] + inc   (mod 2^32),   inc = round(f * 2^32 / fs)
 *   s[n]       = LUT[phase[n] >> (32 - log2(LUT_SIZE))]
 * Frequency resolution fs / 2^32 = 11 uHz at 48 kHz; phase truncation to
 * 10 bits bounds spurs to about -60 dBc.
 *
 * Noise: xorshift32 (period 2^32 - 1), top 16 bits as a uniform Q15 value.
 */
#include "app.h"

#include "app_services.h"
#include "bsp/bsp_timer.h"
#include "fw_tables.h"
#include "sample_pool.h"

static uint32_t s_inc1, s_inc2;
static uint32_t s_ph1, s_ph2;
static q15_t s_a1, s_a2, s_noise;
static uint32_t s_rng = 0x9E3779B9u;
static sample_block_t *s_cur;
static uint16_t s_fill;
static uint8_t s_seq;

static uint32_t phase_inc(uint16_t f_hz)
{
    return (uint32_t)(((uint64_t)f_hz << 32) / APP_FS_HZ);
}

void siggen_configure(uint16_t f1_hz, q15_t a1, uint16_t f2_hz, q15_t a2, q15_t noise)
{
    s_inc1 = phase_inc(f1_hz);
    s_inc2 = phase_inc(f2_hz);
    s_a1 = a1;
    s_a2 = a2;
    s_noise = noise;
    s_ph1 = 0u;
    s_ph2 = 0u;
}

void siggen_start(void)
{
    bsp_sample_timer_start();
}

void siggen_stop(void)
{
    bsp_sample_timer_stop();
    /* The ISR can no longer run, so its half-filled block is ours to return. */
    if (s_cur != NULL) {
        pool_put(s_cur);
        s_cur = NULL;
    }
}

static q15_t next_sample(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    const int32_t t1 = k_sine_lut[s_ph1 >> (32u - SINE_LUT_BITS)];
    const int32_t t2 = k_sine_lut[s_ph2 >> (32u - SINE_LUT_BITS)];
    const int32_t nz = (int16_t)(s_rng >> 16);
    s_ph1 += s_inc1;
    s_ph2 += s_inc2;
    /* Scale each term to Q15 before summing: three Q30 products could overflow int32. */
    int32_t acc = ((t1 * s_a1) >> 15) + ((t2 * s_a2) >> 15) + ((nz * s_noise) >> 15);
    if (acc > 32767) {
        acc = 32767;
    } else if (acc < -32768) {
        acc = -32768;
    }
    return (q15_t)acc;
}

void siggen_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (s_cur == NULL) {
        s_cur = pool_get_from_isr(&woken);
        if (s_cur == NULL) {
            g_stats.siggen_overruns++; /* consumer too slow: sample lost */
            return;
        }
        s_fill = 0u;
    }
    s_cur->in[s_fill++] = next_sample();
    if (s_fill == APP_BLOCK_SIZE) {
        s_cur->len = APP_BLOCK_SIZE;
        s_cur->src = SRC_SIGGEN;
        s_cur->seq = s_seq++;
        const dsp_msg_t m = {.kind = DSP_MSG_BLOCK, .seq = s_cur->seq, .blk = s_cur};
        if (xQueueSendFromISR(g_dsp_q, &m, &woken) != pdTRUE) {
            g_stats.siggen_overruns++;
            pool_put_from_isr(s_cur, &woken);
        }
        s_cur = NULL;
    }
    portYIELD_FROM_ISR(woken);
}
