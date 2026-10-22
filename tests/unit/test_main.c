/**
 * @file test_main.c
 * @brief Host unit tests for libs/dsp and libs/link.
 *
 * Built with -fsanitize=address,undefined so out-of-bounds accesses in the
 * delay lines or the COBS codec fail the test instead of silently passing.
 * Deliberately dependency-free (no Unity/CMocka) so it builds anywhere.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/biquad.h"
#include "dsp/filter_engine.h"
#include "dsp/fir.h"
#include "dsp/fixed_point.h"
#include "link/byteorder.h"
#include "link/cobs.h"
#include "link/crc16.h"
#include "link/frame.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                          \
    } while (0)

#define RUN(test)                                                                                  \
    do {                                                                                           \
        const int before = g_failures;                                                             \
        test();                                                                                    \
        printf("%s %s\n", (g_failures == before) ? "[ OK ]" : "[FAIL]", #test);                    \
    } while (0)

static uint32_t g_rng = 0x12345678u;
static uint32_t rng(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* ------------------------------------------------------------------ link */

static void test_crc16_check_value(void)
{
    const uint8_t msg[] = "123456789";
    CHECK(crc16_ccitt(msg, 9, CRC16_INIT) == CRC16_CHECK);
    CHECK(crc16_ccitt(msg, 0, CRC16_INIT) == CRC16_INIT);
    /* Incremental == one-shot. */
    uint16_t c = crc16_ccitt(msg, 4, CRC16_INIT);
    c = crc16_ccitt(&msg[4], 5, c);
    CHECK(c == CRC16_CHECK);
}

static void test_cobs_roundtrip(void)
{
    static uint8_t src[1200], enc[1300], dec[1300];
    for (int iter = 0; iter < 400; iter++) {
        const size_t len = rng() % 1100u;
        const int mode = iter % 4; /* random / many zeros / no zeros (tests 254-runs) / all zeros */
        for (size_t i = 0; i < len; i++) {
            uint8_t b = (uint8_t)rng();
            if (mode == 1 && (rng() & 1u)) b = 0;
            if (mode == 2 && b == 0) b = 1;
            if (mode == 3) b = 0;
            src[i] = b;
        }
        const size_t n = cobs_encode(src, len, enc, sizeof enc);
        CHECK(n > 0 && n <= COBS_MAX_ENCODED(len));
        int has_zero = 0;
        for (size_t i = 0; i < n; i++) has_zero |= (enc[i] == 0);
        CHECK(!has_zero);
        size_t out = 0;
        CHECK(cobs_decode(enc, n, dec, sizeof dec, &out) == 0);
        CHECK(out == len);
        CHECK(memcmp(src, dec, len) == 0);
        /* in-place decode */
        CHECK(cobs_decode(enc, n, enc, n, &out) == 0 && out == len && memcmp(src, enc, len) == 0);
    }
}

static void test_cobs_rejects_malformed(void)
{
    uint8_t out[16];
    size_t n;
    const uint8_t zero_code[] = {0x00, 0x11};
    const uint8_t truncated[] = {0x05, 0x11, 0x22};
    const uint8_t embedded_zero[] = {0x03, 0x11, 0x00};
    CHECK(cobs_decode(zero_code, sizeof zero_code, out, sizeof out, &n) == -1);
    CHECK(cobs_decode(truncated, sizeof truncated, out, sizeof out, &n) == -1);
    CHECK(cobs_decode(embedded_zero, sizeof embedded_zero, out, sizeof out, &n) == -1);
    const uint8_t ok[] = {0x03, 0x11, 0x22, 0x02, 0x33};
    CHECK(cobs_decode(ok, sizeof ok, out, 2, &n) == -1); /* overflow of dst */
    uint8_t enc[4];
    const uint8_t big[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(cobs_encode(big, sizeof big, enc, sizeof enc) == 0); /* overflow of dst */
}

static void test_frame_roundtrip_and_crc(void)
{
    static uint8_t raw[FRAME_RAW_MAX], wire[FRAME_WIRE_MAX];
    for (size_t i = 0; i < 300; i++) raw[FRAME_PAYLOAD_OFFSET + i] = (uint8_t)(i * 7u);
    const size_t n = frame_finalize(raw, 300, MSG_PROCESS, 42, wire, sizeof wire);
    CHECK(n > 300 && wire[n - 1] == 0);

    uint8_t copy[FRAME_WIRE_MAX];
    memcpy(copy, wire, n);
    frame_t f;
    CHECK(frame_parse(copy, n - 1, &f) == FRAME_OK);
    CHECK(f.type == MSG_PROCESS && f.seq == 42 && f.payload_len == 300);
    CHECK(f.payload[299] == (uint8_t)(299u * 7u));

    /* Any single corrupted bit must be caught (CRC-16 detects all 1-bit errors). */
    for (size_t bit = 0; bit < 64; bit++) {
        memcpy(copy, wire, n);
        copy[5 + bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (copy[5 + bit / 8] == 0) continue; /* would just split the frame */
        CHECK(frame_parse(copy, n - 1, &f) != FRAME_OK);
    }
    CHECK(frame_finalize(raw, LINK_MAX_PAYLOAD + 1, 1, 1, wire, sizeof wire) == 0);
}

/* ------------------------------------------------------------ fixed point */

static void test_fixed_point_conversions(void)
{
    CHECK(dsp_sat_q15(40000) == 32767);
    CHECK(dsp_sat_q15(-40000) == -32768);
    CHECK(dsp_sat_q31((int64_t)1 << 40) == DSP_Q31_MAX);
    CHECK(dsp_q15_to_q31(-1) == -65536);
    CHECK(dsp_q31_to_q15(dsp_q15_to_q31(-12345)) == -12345);
    CHECK(dsp_q31_to_q15(0x7FFFFFFF) == 32767);  /* rounding must not wrap */
    CHECK(dsp_q31_to_q15(0x00008000) == 1);      /* half rounds up */
    CHECK(dsp_f32_to_q15(1.5f) == 32767);
    CHECK(dsp_f32_to_q15(-1.0f) == -32768);
    CHECK(dsp_f32_to_q15(0.5f / 32768.0f) == 1); /* half away from zero */
    CHECK(dsp_f32_to_q15(-0.5f / 32768.0f) == -1);
    for (int v = -32768; v <= 32767; v += 97) {
        CHECK(dsp_f32_to_q15(dsp_q15_to_f32((q15_t)v)) == v);
    }
}

/* -------------------------------------------------------------------- FIR */

static void test_fir_q15_impulse_and_blocks(void)
{
    enum { TAPS = 37, N = 300 };
    static q15_t h[TAPS], st1[2 * TAPS], st2[2 * TAPS], x[N], y1[N], y2[N];
    for (int i = 0; i < TAPS; i++) h[i] = (q15_t)((int)(rng() % 20000u) - 10000);

    fir_q15_t f1, f2;
    fir_q15_init(&f1, h, TAPS, st1);
    /* Impulse of 0x7FFF (just under 1.0) reproduces h within rounding. */
    memset(x, 0, sizeof x);
    x[0] = 32767;
    fir_q15_process(&f1, x, y1, N);
    for (int i = 0; i < TAPS; i++) {
        CHECK(abs(y1[i] - h[i]) <= 1);
    }
    for (int i = TAPS; i < N; i++) CHECK(y1[i] == 0);

    /* Block-size invariance and in-place operation. */
    for (int i = 0; i < N; i++) x[i] = (q15_t)rng();
    fir_q15_init(&f1, h, TAPS, st1);
    fir_q15_init(&f2, h, TAPS, st2);
    fir_q15_process(&f1, x, y1, N);
    memcpy(y2, x, sizeof x);
    size_t pos = 0;
    while (pos < N) {
        size_t n = 1 + rng() % 50u;
        if (pos + n > N) n = N - pos;
        fir_q15_process(&f2, &y2[pos], &y2[pos], n);
        pos += n;
    }
    CHECK(memcmp(y1, y2, sizeof y1) == 0);
}

static void test_fir_f32_matches_double(void)
{
    enum { TAPS = 64, N = 256 };
    static f32_t h[TAPS], st[2 * TAPS], x[N], y[N];
    for (int i = 0; i < TAPS; i++) h[i] = (f32_t)((int)(rng() % 2001u) - 1000) / 8000.0f;
    for (int i = 0; i < N; i++) x[i] = (f32_t)((int)(rng() % 2001u) - 1000) / 1000.0f;
    fir_f32_t f;
    fir_f32_init(&f, h, TAPS, st);
    fir_f32_process(&f, x, y, N);
    double worst = 0.0;
    for (int n = 0; n < N; n++) {
        double ref = 0.0;
        for (int k = 0; k < TAPS && k <= n; k++) ref += (double)h[k] * (double)x[n - k];
        worst = fmax(worst, fabs(ref - (double)y[n]));
    }
    CHECK(worst < 1e-5);
}

/* ----------------------------------------------------------------- biquad */

/* 2nd-order Butterworth low-pass, fc = fs/8, from scipy.signal.butter(2, 0.25). */
static const double k_bw2[5] = {0.09763107293781749, 0.19526214587563498, 0.09763107293781749,
                                -0.9428090415820634, 0.33333333333333337};

static void test_biquad_f32_df2t_vs_df1_double(void)
{
    enum { N = 500 };
    f32_t c[5] = {(f32_t)k_bw2[0], (f32_t)k_bw2[1], (f32_t)k_bw2[2], (f32_t)-k_bw2[3], (f32_t)-k_bw2[4]};
    f32_t st[2], x[N], y[N];
    for (int i = 0; i < N; i++) x[i] = (f32_t)((int)(rng() % 2001u) - 1000) / 1000.0f;
    biquad_f32_t bq;
    biquad_f32_init(&bq, c, 1, st);
    biquad_f32_process(&bq, x, y, N);

    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = 0;
    for (int n = 0; n < N; n++) {
        const double xn = (double)x[n];
        const double yn = k_bw2[0] * xn + k_bw2[1] * x1 + k_bw2[2] * x2 - k_bw2[3] * y1 - k_bw2[4] * y2;
        x2 = x1; x1 = xn; y2 = y1; y1 = yn;
        worst = fmax(worst, fabs(yn - (double)y[n]));
    }
    CHECK(worst < 1e-5);
}

static void test_biquad_q31_block_invariance_and_dc_gain(void)
{
    enum { N = 2000 };
    const uint8_t ps = 1; /* |a1| ~ 0.94 < 2 */
    q31_t c[5];
    for (int i = 0; i < 5; i++) {
        const double v = (i < 3) ? k_bw2[i] : -k_bw2[i];
        c[i] = (q31_t)llround(v * (double)(1LL << (31 - ps)));
    }
    static q31_t st1[4], st2[4], x[N], y1[N], y2[N];
    for (int i = 0; i < N; i++) x[i] = 0x20000000; /* DC step of 0.25 */
    biquad_q31_t a, b;
    biquad_q31_init(&a, c, 1, ps, st1);
    biquad_q31_init(&b, c, 1, ps, st2);
    biquad_q31_process(&a, x, y1, N);
    memcpy(y2, x, sizeof x);
    for (int pos = 0; pos < N; pos += 7) {
        const int n = (pos + 7 > N) ? N - pos : 7;
        biquad_q31_process(&b, &y2[pos], &y2[pos], (size_t)n);
    }
    CHECK(memcmp(y1, y2, sizeof y1) == 0);
    /* Low-pass DC gain is 1: output settles at the input level within a few LSBs of Q31. */
    CHECK(llabs((long long)y1[N - 1] - 0x20000000LL) < 64);
}

/* ---------------------------------------------------------- filter engine */

static size_t make_desc(uint8_t *buf, uint8_t type, uint8_t ps, uint16_t order, const void *coeffs, size_t bytes)
{
    buf[0] = type;
    buf[1] = ps;
    put_le16(&buf[2], order);
    memcpy(&buf[4], coeffs, bytes); /* host is little-endian, like the target */
    return 4 + bytes;
}

static void test_desc_validation(void)
{
    static uint8_t buf[1024];
    static filter_desc_t d;
    q15_t h[4] = {1000, 2000, 3000, 4000};
    size_t n = make_desc(buf, FILTER_FIR_Q15, 0, 4, h, sizeof h);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_OK);
    CHECK(filter_desc_parse(&d, buf, n - 1) == FILTER_ERR_LEN);
    CHECK(filter_desc_parse(&d, buf, 3) == FILTER_ERR_LEN);
    buf[0] = 99;
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_TYPE);

    n = make_desc(buf, FILTER_FIR_Q15, 0, 0, h, 0);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_ORDER);
    n = make_desc(buf, FILTER_FIR_Q15, 0, FILTER_MAX_TAPS + 1, h, 0);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_ORDER);

    /* Unstable: pole at z = 1.05 -> a1 = -1.05, stored negated = +1.05. */
    f32_t unstable[5] = {1.0f, 0.0f, 0.0f, 1.05f, 0.0f};
    n = make_desc(buf, FILTER_BIQUAD_F32, 0, 1, unstable, sizeof unstable);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_UNSTABLE);
    /* Complex pair with radius^2 = a2 = 1.0 (on the unit circle). */
    f32_t marginal[5] = {1.0f, 0.0f, 0.0f, 0.0f, -1.0f};
    n = make_desc(buf, FILTER_BIQUAD_F32, 0, 1, marginal, sizeof marginal);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_UNSTABLE);
    f32_t nan_c[5] = {NAN, 0, 0, 0, 0};
    n = make_desc(buf, FILTER_BIQUAD_F32, 0, 1, nan_c, sizeof nan_c);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_PARAM);

    q31_t q[5] = {1 << 28, 0, 0, (q31_t)(1.9 * (1 << 30)), (q31_t)(-0.95 * (1 << 30))};
    n = make_desc(buf, FILTER_BIQUAD_Q31, 1, 1, q, sizeof q);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_OK);
    n = make_desc(buf, FILTER_BIQUAD_Q31, 8, 1, q, sizeof q);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_PARAM);
    q[3] = (q31_t)(1.99 * (1 << 30));
    q[4] = (q31_t)(-0.995 * (1 << 30)) * -1; /* a2 = -0.995 -> 1 + a2 = 0.005 < |a1| */
    n = make_desc(buf, FILTER_BIQUAD_Q31, 1, 1, q, sizeof q);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_ERR_UNSTABLE);

    n = make_desc(buf, FILTER_BYPASS, 0, 0, h, 0);
    CHECK(filter_desc_parse(&d, buf, n) == FILTER_OK);
}

static void test_engine_bypass_and_saturation(void)
{
    static filter_engine_t e;
    static filter_desc_t d;
    static q15_t x[200], y[200];
    memset(&d, 0, sizeof d);
    d.type = FILTER_BYPASS;
    CHECK(filter_engine_configure(&e, &d) == FILTER_OK);
    for (int i = 0; i < 200; i++) x[i] = (q15_t)rng();
    filter_engine_process(&e, x, y, 200);
    CHECK(memcmp(x, y, sizeof x) == 0);

    /* Gain-2 FIR (two taps of ~1.0) must clip, never wrap. */
    d.type = FILTER_FIR_Q15;
    d.order = 2;
    d.c.fir_q15[0] = 32767;
    d.c.fir_q15[1] = 32767;
    CHECK(filter_engine_configure(&e, &d) == FILTER_OK);
    for (int i = 0; i < 200; i++) x[i] = (i & 1) ? -30000 : 30000;
    x[0] = x[1] = 30000;
    filter_engine_process(&e, x, y, 200);
    CHECK(y[1] == 32767);
    for (int i = 0; i < 200; i++) CHECK(y[i] >= -32768 && y[i] <= 32767);
}

int main(void)
{
    RUN(test_crc16_check_value);
    RUN(test_cobs_roundtrip);
    RUN(test_cobs_rejects_malformed);
    RUN(test_frame_roundtrip_and_crc);
    RUN(test_fixed_point_conversions);
    RUN(test_fir_q15_impulse_and_blocks);
    RUN(test_fir_f32_matches_double);
    RUN(test_biquad_f32_df2t_vs_df1_double);
    RUN(test_biquad_q31_block_invariance_and_dc_gain);
    RUN(test_desc_validation);
    RUN(test_engine_bypass_and_saturation);
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
