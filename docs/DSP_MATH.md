# The mathematics of RTDSP-F4, and where each equation runs

This document derives every equation the project uses and then pins each one
to the exact place it executes: on the host in Python, at build time, or on
the emulated STM32F405 (in a specific task, ISR or kernel, at a specific rate).
The numbers quoted are from the actual designs in
[`python/rtdsp/catalog.py`](../python/rtdsp/catalog.py) and the firmware in this repository.

---

## 0. Map: equation → location → rate

| # | Equation | Where it runs | When / how often |
|---|---|---|---|
| 2.1 | Sampling `t = n·Ts`, `Ts = 1/fs` | TIM2 update ISR (HW) / SysTick burst (QEMU) | 48 000×/s |
| 2.3 | Block latency `L = N_b / fs` | DSP task period | 1 500 blocks/s |
| 3.x | Q15/Q31 conversions, rounding, saturation | `fixed_point.h` inside `filter_engine_process()` | per sample |
| 4.1 | FIR convolution `y[n] = Σ h[k] x[n−k]` | `fir_q15_process()` / `fir_f32_process()` in the **DSP task** | per sample × N taps |
| 4.4 | Kaiser order & β | `design.kaiser_fir()` on the host | once per design |
| 5.1 | Biquad `H(z)` and difference equation | `biquad_q31_process()` / `biquad_f32_process()` in the **DSP task** | per sample × stages |
| 5.3 | Stability triangle `|a2| < 1, |a1| < 1 + a2` | `filter_desc_validate()` in **link_rx task** (and in Python) | per SET_FILTER |
| 5.5 | post-shift / overflow bound | `quantize.choose_post_shift()` on the host | once per design |
| 5.6 | L∞ section scaling | `quantize.scale_sos_linf()` on the host | once per design |
| 5.7 | Bilinear transform + pre-warp | `scipy.signal.iirfilter` (host) | once per design |
| 5.8 | RBJ notch | `design.notch_sos()` (host) | once per design |
| 7.1 | NCO phase accumulator | `siggen.c` in the **sample-clock ISR** | per sample |
| 7.3 | xorshift32 noise / POST stimulus | `siggen.c` ISR; `post.c` at boot; `fixedpoint.xorshift32_q15()` (host) | per sample / once at boot |
| 8.1 | Cycle budget `f_cpu / fs` | `app_info.c` (reported); `supervisor.c` (logged) | on GET_STATS / every 5 s |
| 8.2 | Rate-monotonic priorities and response time | `app_config.h` (design-time) | compile time |
| 8.4 | EMA of cycles/sample | `dsp_task.c` `update_timing()` | per block |
| 8.5 | CPU load from idle time | `supervisor.c` | every 500 ms |
| 8.6 | Timer calibration `f_tim = Δticks / Δcycles · f_cpu` | `bsp_timer_calibrate()` before the scheduler starts | once at boot |
| 9.1 | CRC-16/CCITT | `crc16.c`: link_rx (verify), link_tx (generate), POST | per frame |
| 9.2 | COBS overhead `⌈n/254⌉` | `cobs.c`: link_rx / link_tx | per frame |
| 10.x | Welch transfer function, SQNR, single-bin DFT | `analysis.py` / `demo.py` on the host | per test |

---

## 1. The signal path at a glance

```
host (Python)                     emulated STM32F405 (FreeRTOS)
─────────────                     ──────────────────────────────────────────────────────────
design (float64)                  USART1 ISR ─► rx stream buf ─► link_rx task
  │ bilinear / Kaiser               │                               │ COBS⁻¹, CRC check
quantise (Q15/Q31/f32)              │                               │ SET_FILTER: validate (5.3),
  │ post_shift, L∞ scaling          │                               │   build inactive bank
wire descriptor ──── COBS+CRC ────► │                               ▼
                                    │                         DSP queue ◄── sample-clock ISR
test signal x[n] ── PROCESS ──────► │                               │        (NCO 7.1 + noise 7.3)
                                    │                               ▼
                                    │                     DSP task: y = H{x}  (4.1 / 5.1)
y[n] ◄──── PROCESS_RESULT ───────── link_tx task ◄── TX queue ◄─────┘
  │                                                   (frames, CRC 9.1, COBS 9.2)
compare: bit-exact model, SQNR, |H| (10.x)
```

---

## 2. Sampling and blocks

### 2.1 Sampling
A continuous signal `x(t)` sampled every `Ts = 1/fs` gives `x[n] = x(nTs)`. With
**fs = 48 kHz**, `Ts = 20.83 µs`. By the Nyquist–Shannon theorem everything
above `fs/2 = 24 kHz` aliases, so every design frequency here lies in `(0, 24 kHz)`
and the frequency axis of every plot ends at 24 kHz.

The digital frequency used by every transfer function below is

```
ω = 2π f / fs        (radians/sample, ω ∈ [0, π] ↔ f ∈ [0, fs/2])
```

**On the board.** On silicon, TIM2 generates one update interrupt per sample:
`f_update = f_tim / ((PSC+1)(ARR+1))`, so `ARR = round(f_tim/fs) − 1`
(`bsp_timer.c`). With `f_tim = 84 MHz`, `ARR = 1749`, which gives exactly 48 000.0 Hz.
On QEMU the sample clock is SysTick-derived (see ARCHITECTURE.md §7). A
Bresenham accumulator emits exactly `fs` samples per second even when
`fs / f_tick` is not an integer:

```
phase += fs;  while (phase ≥ f_tick) { phase −= f_tick; emit_sample(); }
```

### 2.2 Why blocks
Processing one sample per interrupt costs a context switch per 20.8 µs.
Instead, samples are gathered into **blocks of N_b = 32**, the equivalent of a
DMA half-transfer. The DSP task then wakes once per block period:

```
T_block = N_b / fs = 32 / 48 000 = 666.7 µs      (1 500 wake-ups/s instead of 48 000)
```

### 2.3 Latency
Block processing adds a fixed latency of one block on input and one on output:
`L ≈ 2·N_b/fs = 1.33 ms`, plus the filter's own group delay (§4.3). That is
the price of the context-switch saving in §2.2.

---

## 3. Number formats (fixed point)

### 3.1 Q format
A `Q1.(B−1)` number stores a B-bit two's-complement integer `I` and represents

```
v = I / 2^(B−1),      v ∈ [−1, 1 − 2^−(B−1)]
```

* **Q15** (`int16_t`): resolution `q = 2^−15 ≈ 3.05·10⁻⁵`. This is the I/O format,
  the same one a 16-bit ADC/DAC delivers.
* **Q31** (`int32_t`): resolution `2^−31 ≈ 4.66·10⁻¹⁰`. This is the internal format of the IIR kernel.

### 3.2 Products and accumulators
`Qa × Qb = Q(a+b)`: Q15·Q15 → Q30 (fits 32 bits), Q31·Q31 → Q62 (needs 64).
Summing `N` products can grow the magnitude by `N`, so the accumulator needs
`⌈log₂ N⌉` **guard bits**. The FIR uses a 64-bit accumulator: Q30 products
leave 33 guard bits, against the 7 needed for 128 taps. Intermediate overflow
is therefore impossible, and only the final store saturates.

**On the board:** `int64_t acc += (int32_t)h[k] * (int32_t)w[k]` in `fir.c`
compiles to Cortex-M4 `SMLAL` (signed multiply-accumulate long, single-cycle issue).

### 3.3 Re-quantisation (rounding)
Returning from a wide accumulator to the output format means dropping bits.
Truncation (`acc >> s`) has a mean error of −½ LSB, which shows up as a DC bias.
Round-half-up removes the bias:

```
y = (acc + 2^(s−1)) >> s
```

`fir_q15_process` uses `s = 15` (Q30 → Q15). `biquad_q31_process` uses
`s = 31 − post_shift`. `dsp_q31_to_q15` uses `s = 16` and computes
`(x + 0x8000) >> 16` in 64 bits so that rounding `0x7FFFFFFF` cannot wrap.

### 3.4 Quantisation noise
Rounding to step `q` adds an error that is (for busy signals) uniform on
`[−q/2, q/2]`, with variance

```
σ_e² = q² / 12
```

For a full-scale sine (`P_s = 1/2`) and B bits (`q = 2^{1−B}`) that gives the
classic result

```
SQNR = 10 log₁₀( P_s / σ_e² ) = 6.02·B + 1.76 dB   →  98.1 dB for B = 16
```

The SQNRs measured on the board (74.7–88.2 dB, see the demo report) are
lower for three reasons: the test signal is at −12 dB, not full scale; the
filter's noise gain `Σ h²` shapes the output noise; and each IIR section
re-quantises internally.

### 3.5 Saturation vs wrap-around
Two's-complement overflow wraps: `32767 + 1 = −32768`, a full-scale jump.
Inside a recursive filter that jump feeds back and can sustain an
**overflow limit cycle**. Every narrowing conversion therefore clamps:

```
sat(v) = min(max(v, −2^(B−1)), 2^(B−1) − 1)
```

(`dsp_sat_q15`, `dsp_sat_q31`; GCC lowers the pattern to the M4 `SSAT` instruction.)

### 3.6 Worst-case gain of an FIR (when clipping is possible)
For any input with `|x| ≤ 1`:

```
|y[n]| = |Σ h[k] x[n−k]| ≤ Σ |h[k]| = ‖h‖₁
```

For the default low-pass, `‖h‖₁ = 1.745`. A worst-case input can therefore
reach 1.75 × full scale, and the output saturates instead of wrapping. This is
a deliberate trade-off: scaling h by 1/1.745 would cost 4.8 dB of SNR on every
normal signal to protect against one pathological input.
`quantize.fir_l1_gain()` reports this number.

---

## 4. FIR filters

### 4.1 Difference equation and transfer function
```
y[n] = Σ_{k=0}^{N−1} h[k] · x[n−k]

H(z) = Σ_{k=0}^{N−1} h[k] z^{−k},        H(e^{jω}) = Σ h[k] e^{−jωk}
```
H has only zeros (poles at the origin), so an FIR is **always stable**. The
firmware validates only its length.

**On the board** (`fir.c`, DSP task): N MACs per sample. The delay line is a
*mirrored circular buffer* of 2N samples. Each new sample is written at
`idx` and at `idx+N`, so the window `x[n], x[n−1], …, x[n−N+1]` is always the
contiguous run `state[idx … idx+N−1]`:

```
idx ← (idx − 1) mod N;   state[idx] = state[idx+N] = x[n];   y[n] = Σ h[k]·state[idx+k]
```

The inner loop has no modulo and no wrap test. The cost is one extra store per
sample and N extra words of RAM.

### 4.2 Linear phase
If `h[k] = h[N−1−k]` (symmetric, N odd: "type I"), then

```
H(e^{jω}) = e^{−jω(N−1)/2} · A(ω),    A(ω) real
```

so every frequency is delayed by the same **group delay**
`τ_g = −dφ/dω = (N−1)/2` samples, and the waveform shape is preserved. Q15
rounding is applied symmetrically, so the quantised taps stay exactly
symmetric (asserted in `test_design.py`).

### 4.3 Windowed-sinc design
The ideal low-pass with cut-off `f_c` has the infinite impulse response
`h_d[n] = 2f_c/fs · sinc(2f_c/fs · (n − M))`, with `M = (N−1)/2`. Truncating it
to N taps convolves the response with the window's spectrum, which causes
Gibbs ripple. A window `w[n]` trades main-lobe width (transition band) for
side-lobe level (stop-band attenuation):

```
h[n] = w[n] · h_d[n]
```

Hamming (`w[n] = 0.54 − 0.46 cos(2πn/(N−1))`, about 53 dB) is used for the
band-pass. The default low-pass uses Kaiser.

### 4.4 Kaiser window and order estimate
The Kaiser window
`w[n] = I₀(β √(1 − (2n/(N−1) − 1)²)) / I₀(β)` (`I₀` is the modified Bessel
function) has a continuous trade-off parameter β. Kaiser's empirical formulas,
for stop-band attenuation `A` in dB and transition width `Δω = 2π Δf / fs`:

```
β = 0.1102 (A − 8.7)                                  A > 50
  = 0.5842 (A − 21)^0.4 + 0.07886 (A − 21)            21 ≤ A ≤ 50
N ≈ (A − 7.95) / (2.285 · Δω) + 1
```

For the boot filter (`A = 60 dB`, pass band to 4 kHz, stop band from 6 kHz,
so `Δf = 2 kHz`): **β = 5.653, N = 89 taps**, and the group delay is
`44 samples = 0.917 ms`.

### 4.5 Coefficient quantisation of an FIR
Rounding each tap adds `ε[k]`, with `|ε[k]| ≤ 2^−16`. The response error is
bounded by

```
|ΔH(e^{jω})| = |Σ ε[k] e^{−jωk}| ≤ N · 2^−16 = 89 · 1.5·10⁻⁵ ≈ 1.4·10⁻³  (−57 dB worst case)
```

The statistical estimate is `√N · 2^−16/√3 ≈ −94 dB`. Measured from the Q15
taps, the stop band reaches −59.98 dB (spec 60 dB), which is consistent with
the error landing on the stop-band ripple peaks. `test_design.py` asserts
better than −57 dB.

---

## 5. IIR filters (biquad cascades)

### 5.1 The biquad
```
           b0 + b1 z⁻¹ + b2 z⁻²
H_k(z) = ─────────────────────── ,      H(z) = Π_k H_k(z)
           1  + a1 z⁻¹ + a2 z⁻²
```
Its difference equation, **Direct Form I** (the Q31 kernel, `biquad_q31_process`):
```
y[n] = b0 x[n] + b1 x[n−1] + b2 x[n−2] − a1 y[n−1] − a2 y[n−2]
```
**Direct Form II Transposed** (the float kernel, `biquad_f32_process`), with two states:
```
y[n]  = b0 x[n] + s1
s1    = b1 x[n] − a1 y[n] + s2
s2    = b2 x[n] − a2 y[n]
```
Both forms compute the same H(z). They differ in rounding behaviour:

* **DF1 for fixed point.** Its only internal node is the accumulator (64-bit,
  §5.5), so nothing can overflow between the zeros and the poles. It needs 4
  states per section.
* **DF2T for float.** It needs only 2 states, and in floating point the
  internal-node range problem does not exist.

Coefficients travel and are stored as `{b0, b1, b2, −a1, −a2}`: negating the
feedback terms makes every term a multiply-*add*, which is also CMSIS-DSP's
layout.

**On the board** (DSP task) the loop is stage-major: the whole block runs
through stage 0, then stage 1, and so on. The 5 coefficients and the states
of a stage then stay in registers for the whole block. Cost: 5 MACs per
sample per section.

### 5.2 Poles and zeros
The poles are the roots of `z² + a1 z + a2 = 0`:

```
p₁,₂ = ( −a1 ± √(a1² − 4a2) ) / 2
```

For a complex pair `p = r e^{±jθ}`: `a1 = −2r cos θ`, `a2 = r²`. The pole
radius `r` sets the resonance sharpness (`Q ≈ 1/(2(1−r))` for r near 1), and
θ sets its frequency `f = θ fs / 2π`.

### 5.3 Stability: the stability triangle
A causal LTI system is BIBO-stable iff all poles satisfy `|p| < 1`. For the
quadratic above, the Jury/Schur–Cohn conditions reduce to

```
|a2| < 1     and     |a1| < 1 + a2
```

which describes a triangle in the `(a1, a2)` plane.

*Derivation sketch:* `|a2| = |p₁p₂| < 1` is necessary. With `D(z) = z² + a1z + a2`, the
real roots stay inside (−1, 1) iff `D(1) = 1 + a1 + a2 > 0` and
`D(−1) = 1 − a1 + a2 > 0`, i.e. `|a1| < 1 + a2`. For complex roots `r² = a2 < 1` is the whole condition.

**On the board:** `filter_desc_validate()` runs this test in the **link_rx
task** on every SET_FILTER, before the new bank can become active. In the Q31
case it runs in pure integers, with "1.0" represented as `2^(31−post_shift)`.
An unstable upload is answered with `ST_ERR_UNSTABLE`, and the running filter
is untouched (tested in `test_pil.py::test_unstable_filter_rejected_and_previous_kept`).

### 5.4 Why a cascade of sections, not one high-order polynomial
Pole sensitivity to a coefficient error grows with how close the poles are
to each other:

```
∂p_i / ∂a_k = − p_i^{N−k} / Π_{j≠i} (p_i − p_j)
```

High-order filters have tightly clustered poles, so the denominator is tiny
and the sensitivity huge. Measured on this project's 6th-order elliptic
(dominant pole radius 0.979911):

| coefficients quantised to 16 bits as… | dominant pole radius |
|---|---|
| one 6th-order denominator (direct form) | **0.99128** (Q roughly ×2.3, response wrecked) |
| three biquads (this design) | **0.979901** (error 10⁻⁵) |

That is why the IIR path accepts SOS only. `scipy`'s `zpk2sos` pairs each
pole pair with its nearest zeros and orders the sections so that the
highest-Q poles come last, where the most-filtered signal reaches them.

### 5.5 The post-shift and a proof that the accumulator cannot overflow
IIR coefficients routinely exceed 1: `|a1| < 2` for a stable section, and
the elliptic stage 3 has `a1 = −1.808`. Q31 holds only `[−1, 1)`. So every
coefficient is stored scaled by `2^−s` (**post_shift** s):

```
c_int = round( c · 2^(31−s) )                         (Q(31−s) format)
y = sat₃₂( (Σ c_int · v + 2^(30−s)) >> (31 − s) )     (Q31 · Q(31−s) = Q(62−s) → Q31)
```

**Choice of s** (`quantize.choose_post_shift`) is the smallest s satisfying, for every section,

```
(i)  max |c| < 2^s                (each coefficient is representable)
(ii) Σ |c|   < 2^(1+s)            (the 64-bit accumulator cannot overflow)
```

*Proof of (ii):* every data value `|v| ≤ 2^31`, and
`|c_int| ≤ |c|·2^(31−s)`. Then
`|acc| ≤ Σ|c|·2^(31−s)·2^31 = Σ|c|·2^(62−s) < 2^(1+s)·2^(62−s) = 2^63`. ∎

The firmware can therefore use a plain `int64_t` sum with no overflow check
in the inner loop, and C's undefined signed overflow cannot occur. The
Python reference model asserts the same bound on every sample.

For the elliptic design: `Σ|c|` per section = 2.54, 3.37, 4.57, so **s = 2**
(Q29 coefficients, still 29 fractional bits, about 1.9·10⁻⁹ resolution).

### 5.6 L∞ scaling between sections
Even with a safe accumulator, an *intermediate* section's output can exceed
full scale and saturate, which is distortion. For each internal node k,
`scale_sos_linf` computes the cumulative response
`G_k(e^{jω}) = Π_{i≤k} H_i(e^{jω})` and scales section k's numerator by

```
g_k = 1 / max_ω |G_k(e^{jω})|
```

so that a full-scale sinusoid at *any* frequency stays ≤ full scale at every
node. The last section absorbs `1/Π g_k`, so the overall `H(z)` is unchanged
(asserted to 10⁻⁹ in `test_design.py`).

### 5.7 From analog prototype to digital: the bilinear transform
The Butterworth, Chebyshev and elliptic families are defined in the analog domain:

```
Butterworth  |H(jΩ)|² = 1 / (1 + (Ω/Ω_c)^{2N})                     maximally flat
Chebyshev I  |H(jΩ)|² = 1 / (1 + ε² T_N²(Ω/Ω_c))                   equiripple pass band
Elliptic     |H(jΩ)|² = 1 / (1 + ε² R_N²(Ω, ξ))                    equiripple both bands
```

(`T_N`: Chebyshev polynomial, `R_N`: elliptic rational function, ε sets the
ripple `R_p = 10 log₁₀(1+ε²)` dB). The **bilinear transform** maps the
s-plane to the z-plane:

```
s = 2·fs · (1 − z⁻¹) / (1 + z⁻¹)
```

It maps the whole jΩ axis onto the unit circle exactly once, so there is no
aliasing, and it maps the left half-plane inside the unit circle, so stable
prototypes stay stable. The cost is a non-linear frequency **warping**
`Ω = 2 fs tan(ω/2)`. Band edges are therefore **pre-warped** before the
analog design:

```
Ω_c = 2·fs · tan(π f_c / fs)
```

so the digital edge lands exactly on `f_c`. scipy's `iirfilter(..., fs=fs)` does
this. It runs on the host, and only the resulting SOS reaches the board.

### 5.8 The notch (RBJ Audio-EQ cookbook)
A notch at `f0` puts zeros exactly on the unit circle at `±ω0`, and poles at the
same angle just inside it:

```
ω0 = 2π f0 / fs,   α = sin ω0 / (2Q)

        1 − 2cos ω0 z⁻¹ + z⁻²
H(z) = ──────────────────────────────────────
       (1+α) − 2cos ω0 z⁻¹ + (1−α) z⁻²
```

The pole radius is `r = √((1−α)/(1+α))`. For f0 = 1 kHz and Q = 10:
`r = 0.993495`, and the −3 dB bandwidth is `f0/Q = 100 Hz`. The zeros sit on
the circle, so the theoretical depth is infinite. On the board the 1 kHz tone
is attenuated by more than 40 dB (asserted) while 3 kHz passes within 0.5 dB.
This is the classic 50/60 Hz mains-hum remover.

---

## 6. The float32 path, and how it can be bit-exact

IEEE-754 binary32 has a 24-bit significand, so its machine epsilon is
`2^−24 ≈ 6·10⁻⁸`, about 144 dB below full scale. Precision is not the issue.
**Reproducibility** is: `a·b + c` can be evaluated as two rounded operations or
as one fused multiply-add (FMA, a single rounding), and the two can differ in
the last bit. GCC and Clang fuse by default (`-ffp-contract`). The project
compiles the firmware *and* the host SIL library with `-ffp-contract=off`, and
`fixedpoint.biquad_f32` evaluates in exactly the C order, rounding to float32
after every operation. The result is that all three implementations (Python
model, host C, Cortex-M4 FPU under QEMU) produce **bit-identical** outputs.
This is asserted for every sample in `test_sil.py` and `test_pil.py`.

Output conversion uses round half away from zero, with the clamp done in the
float domain first (out-of-range float→int casts are undefined behaviour in C):

```
s = y·32768;   q = (s ≥ 32767) ? 32767 : (s ≤ −32768) ? −32768 : trunc(s ± 0.5)
```

---

## 7. The on-board signal generator

### 7.1 Numerically controlled oscillator
```
φ[n+1] = (φ[n] + Δφ) mod 2³²,        Δφ = round(f · 2³² / fs)
s[n]   = LUT[ φ[n] >> (32 − 10) ]    (1024-entry Q15 sine table)
```
The 32-bit unsigned overflow *is* the mod 2π. The frequency resolution is
`fs / 2³² = 11.2 µHz`.

**On the board:** this runs in the sample-clock **ISR** (`siggen.c`), once per
sample. `Δφ` is computed once in `siggen_configure()` with a 64-bit divide
(`__aeabi_uldivmod` from libgcc). That divide is kept out of the ISR.

### 7.2 Spur level from phase truncation
Keeping only the top P = 10 bits of phase introduces a periodic phase error
of up to `2π/2^P`. The resulting spurs sit about `6.02·P ≈ 60 dBc` below the
carrier. The table itself is produced on the host by
`gen_firmware_tables.py` as `round(32767 sin(2πi/1024))`.

### 7.3 Pseudo-random noise: xorshift32
```
x ^= x << 13;  x ^= x >> 17;  x ^= x << 5        (period 2³² − 1)
```
The top 16 bits are taken as a Q15 sample, uniform on [−1, 1). The same
generator, seeded with `0x1234ABCD`, produces the 256-sample **POST**
stimulus. Python (`fixedpoint.xorshift32_q15`) reproduces it bit for bit to
compute the golden CRCs that are compiled into the firmware.

### 7.4 Mixing
```
s = sat16( (t1·a1 ≫ 15) + (t2·a2 ≫ 15) + (noise·a_n ≫ 15) )
```
Each Q15×Q15 product is renormalised before the sum. Three Q30 products
summed directly could exceed `2^31` and overflow `int32`.

---

## 8. Real-time and RTOS mathematics

### 8.1 Cycle budget and utilisation
```
cycles available per sample  C_s = f_cpu / fs = 168 MHz / 48 kHz = 3 500
utilisation                  U   = (measured cycles per sample) / C_s
```
Approximate per-sample cost on a Cortex-M4 at 0 wait states (CCM):

| kernel | operations per sample | rough cycles |
|---|---|---|
| FIR Q15, 89 taps | 89 SMLAL + loads | ~180–270 |
| Biquad Q31, 3 sections | 15 SMLAL + shifts | ~60–80 |
| Biquad f32, 2 sections | 10 VMUL/VADD | ~40–60 |

On silicon, `DWT->CYCCNT` measures the real number. QEMU is not
cycle-accurate: its figure comes from a scaled emulated timer and is
indicative only. All the designs use under 10 % of the budget, leaving the
rest for the communication tasks and for growth.

### 8.2 Task priorities: rate-monotonic assignment
Rate-monotonic scheduling (RMS) assigns higher priority to shorter periods,
and it is optimal among fixed-priority schemes. The Liu & Layland bound says
n periodic tasks are schedulable if

```
U_total = Σ C_i / T_i ≤ n (2^{1/n} − 1)        (n = 3 → 0.780, n → ∞ → ln 2 = 0.693)
```

| task | period T | prio |
|---|---|---|
| DSP | 0.667 ms (block) | 6 (highest) |
| link_rx | driven by bytes, ~frame time | 5 |
| link_tx | ~frame time | 4 |
| supervisor | 500 ms | 2 |
| log | best effort | 1 |

### 8.3 Response-time analysis (exact test)
For the DSP task, the only higher-priority interference comes from ISRs. Its
worst-case response time `R` is the fixed point of

```
R = C_dsp + Σ_{j ∈ ISRs} ⌈R / T_j⌉ · C_j
```

and the task is schedulable if `R ≤ T_block`. With the 89-tap FIR
(`C_dsp ≈ 32 × 270 ≈ 8 600` cycles ≈ 51 µs), the sample ISR (≈150 cycles every
20.8 µs), and the UART byte ISR (≈200 cycles every 10.8 µs at 921 600 baud),
iteration converges at roughly `R ≈ 130 µs ≪ 667 µs`.

### 8.4 Moving average of cost
The DSP task tracks cycles/sample in Q8 fixed point with an exponential moving average:

```
avg ← avg + (x − avg) / 16           (α = 1/16, time constant ≈ 16 blocks)
```

This needs no division by a running count and has no window buffer.

### 8.5 CPU load
The FreeRTOS run-time counter (fed from `bsp_timestamp()`) accumulates the
idle task's time. Over each 500 ms supervisor window:

```
load = 100 · (1 − Δt_idle / Δt_total)
```

The computation uses unsigned differences, so it is correct across counter
wrap-around as long as the window is shorter than one wrap period.

### 8.6 Pool sizing and back-pressure
Blocks circulate ISR → DSP → TX → pool. By Little's law,
`blocks in flight = arrival rate × time in system`. With 8 blocks of 0.667 ms,
the pipeline absorbs `8 × 0.667 = 5.3 ms` of TX stall before the ISR finds
the pool empty. At that point it counts an **overrun** rather than block,
because an ISR must never wait.

### 8.7 Timer self-calibration
Before the scheduler starts, the TIM2 count is measured over a known number
of SysTick core cycles:

```
f_tim = Δticks_TIM2 / Δcycles_SysTick · f_cpu
```

On silicon this must equal `84 MHz ± 2 %`, otherwise POST reports
`POST_FAIL_CLOCK`. That turns a wrong PLL or prescaler setting into a boot
error instead of a wrong sample rate in the field. Under QEMU it measures the
emulator's ~1 GHz timer model.

---

## 9. Communication mathematics

### 9.1 CRC-16/CCITT-FALSE
Frames are protected by the remainder of polynomial division over GF(2):

```
g(x) = x¹⁶ + x¹² + x⁵ + 1   (0x1021),   init 0xFFFF,   check("123456789") = 0x29B1
```

g(x) has the factor `(x+1)`. It therefore detects all odd-weight errors, all
1- and 2-bit errors in frames this size, and every burst of length ≤ 16. A
random corruption goes undetected with probability `2^−16`. The
implementation processes a nibble per table lookup: a 16-entry table, 32
bytes of flash.

### 9.2 COBS
Consistent Overhead Byte Stuffing removes every 0x00 from the payload, so
0x00 can delimit frames. Its overhead is bounded:

```
encoded length ≤ n + ⌈n/254⌉ + 1
```

That is 0.4 % worst case against up to 100 % for byte-escaping (SLIP/HDLC).
A receiver that joins mid-stream resynchronises at the next 0x00
(`test_protocol.py::test_frame_reader_resyncs_after_garbage`).

### 9.3 Link throughput (sizing the host-stream mode)
A 64-sample PROCESS frame carries 128 payload bytes. Adding 4 bytes of
header/CRC, 1 byte of COBS overhead and 1 delimiter gives 134 bytes. At
921 600 baud (10 bits per byte on the wire, 92 160 B/s):

```
92 160 / 134 = 688 frames/s × 64 = 44 000 samples/s each way
```

which is just under 48 kHz. Real-time streaming of a live 48 kHz signal
from a PC therefore needs a faster link (USB-CDC, or 2 Mbaud). That is why the
production-style path is the on-board ISR source (the ADC on a real board),
and why the PC path is used for verification rather than live audio.

---

## 10. Measurement mathematics (host side)

* **Transfer function from data** (`demo.py`): with Welch-averaged spectra,
  `Ĥ(f) = S_xy(f) / S_xx(f)`. This is the curve marked "measured on emulated
  STM32" in the plots. It overlays the design curve to within the variance of
  the estimate.
* **SQNR** (`analysis.sqnr_db`): `10 log₁₀( Σ y_ref² / Σ (y − y_ref)² )`, where
  `y_ref` is the float64 output of the *unquantised* design. It measures
  everything the embedded implementation costs.
* **Tone gain** (`analysis.tone_gain_db`): a single-bin DFT,
  `X(f) = Σ x[n] e^{−j2πfn/fs}`, computed on the second half of the record
  so the start-up transient is excluded. Gain is `20 log₁₀ |Y(f)/X(f)|`.

---

## 11. One sample, end to end (elliptic Q31 low-pass)

1. **Host:** `iirfilter('ellip', 6, 3000, rp=0.5, rs=60)` pre-warps 3 kHz
   (§5.7), then designs and bilinear-transforms. The result is 3 SOS with
   pole radii 0.876, 0.930, 0.980.
2. **Host:** `scale_sos_linf` (§5.6), then `choose_post_shift` → s = 2 (§5.5).
   Coefficients become Q29: stage 3 `−a1 = 1.8083 → 970 824 621`.
3. **Wire:** `[03 02 03 00][15 × int32 LE]` → CRC-16 (§9.1) → COBS (§9.2) → USART1.
4. **Board, link_rx:** COBS⁻¹, CRC ✓. Parse, then check the stability triangle
   on each stage (§5.3): stage 3 `|a2| = 0.960 < 1`, `|a1| = 1.808 < 1.960` ✓.
   Build the inactive bank, then queue SWAP.
5. **Board, DSP task:** at the next block boundary `active ^= 1`, then ACK.
6. **Board, per sample:** `x_q15 → x_q31 = x·2^16` (§3.1). Three DF1 sections, each
   `acc = Σ c·v` (64-bit, provably no overflow), `y = sat((acc + 2^28) >> 29)`.
   Then `y_q15 = sat((y + 2^15) >> 16)` (§3.3).
7. **Host:** the int16 stream equals `fixedpoint.biquad_q31(...)` exactly. SQNR
   against the float64 design is 84.7 dB. The Welch estimate matches |H|
   down to the −60 dB stop band.
