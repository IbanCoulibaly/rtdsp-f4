"""Bit-exact Python models of libs/dsp/src/filter_engine.c.

Each function reproduces the C arithmetic operation for operation, including
rounding, saturation and (for float32) the evaluation order, so that the
SIL library, the firmware running in QEMU and these models produce
*identical* int16 output. The C code is compiled with -ffp-contract=off so
the compiler cannot fuse a*b+c into an FMA (which rounds once instead of
twice and would break bit-exactness against this model).
"""

from __future__ import annotations

import numpy as np

from .quantize import (BIQUAD_F32, BIQUAD_Q31, BYPASS, FIR_F32, FIR_Q15,
                       FilterDesc)

F32 = np.float32
INV_32768 = F32(1.0 / 32768.0)
I64_MIN, I64_MAX = -(2 ** 63), 2 ** 63 - 1


def sat(v: int, bits: int) -> int:
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return lo if v < lo else hi if v > hi else v


def q15_to_f32(x: np.ndarray) -> np.ndarray:
    return x.astype(F32) * INV_32768


def f32_to_q15(y: np.ndarray) -> np.ndarray:
    """dsp_f32_to_q15: scale, clamp, then round half away from zero in float32."""
    s = y.astype(F32) * F32(32768.0)
    out = np.empty(len(s), dtype=np.int16)
    hi = s >= F32(32767.0)
    lo = s <= F32(-32768.0)
    mid = ~(hi | lo)
    sm = s[mid]
    r = np.where(sm >= F32(0.0), sm + F32(0.5), sm - F32(0.5)).astype(F32)
    out[mid] = np.trunc(r).astype(np.int16)
    out[hi] = 32767
    out[lo] = -32768
    return out


def fir_q15(h: np.ndarray, x: np.ndarray) -> np.ndarray:
    """y = sat16((sum h[k] x[n-k] + 2^14) >> 15); int64 is exact here (|acc| < 2^38)."""
    acc = np.convolve(x.astype(np.int64), h.astype(np.int64))[: len(x)]
    return np.clip((acc + (1 << 14)) >> 15, -32768, 32767).astype(np.int16)


def fir_f32(h: np.ndarray, x: np.ndarray) -> np.ndarray:
    """acc = 0; for k: acc = acc + h[k]*x[n-k], each op rounded to float32."""
    h = h.astype(F32)
    xf = q15_to_f32(x)
    n = len(xf)
    acc = np.zeros(n, dtype=F32)
    for k in range(len(h)):
        shifted = np.concatenate([np.zeros(min(k, n), dtype=F32), xf[: max(n - k, 0)]])
        acc = (acc + (h[k] * shifted).astype(F32)).astype(F32)
    return f32_to_q15(acc)


def biquad_q31(coeffs: np.ndarray, post_shift: int, x: np.ndarray) -> np.ndarray:
    """Direct Form I, Q31 data, Q(31-ps) coefficients, 64-bit accumulator."""
    shift = 31 - post_shift
    rnd = 1 << (shift - 1)
    sig = [int(v) * 65536 for v in x]  # q15 -> q31
    for b0, b1, b2, na1, na2 in (tuple(int(c) for c in row) for row in coeffs):
        x1 = x2 = y1 = y2 = 0
        out = []
        for xn in sig:
            acc = b0 * xn + b1 * x1 + b2 * x2 + na1 * y1 + na2 * y2
            if not I64_MIN <= acc <= I64_MAX:  # the post_shift rule guarantees this never fires
                raise OverflowError("64-bit accumulator overflow")
            yn = sat((acc + rnd) >> shift, 32)
            x2, x1, y2, y1 = x1, xn, y1, yn
            out.append(yn)
        sig = out
    return np.array([sat((v + 0x8000) >> 16, 16) for v in sig], dtype=np.int16)


def biquad_f32(coeffs: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Direct Form II Transposed in float32, same operation order as the C."""
    sig = [F32(v) for v in q15_to_f32(x)]
    for b0, b1, b2, na1, na2 in (tuple(F32(c) for c in row) for row in coeffs.astype(F32)):
        s1 = s2 = F32(0.0)
        out = []
        for xn in sig:
            y = F32(F32(b0 * xn) + s1)
            s1 = F32(F32(F32(b1 * xn) + F32(na1 * y)) + s2)
            s2 = F32(F32(b2 * xn) + F32(na2 * y))
            out.append(y)
        sig = out
    return f32_to_q15(np.array(sig, dtype=F32))


def engine(desc: FilterDesc, x: np.ndarray) -> np.ndarray:
    """Model of filter_engine_process() for any descriptor, from reset state."""
    x = np.asarray(x, dtype=np.int16)
    if desc.type == BYPASS:
        return x.copy()
    if desc.type == FIR_Q15:
        return fir_q15(desc.coeffs, x)
    if desc.type == FIR_F32:
        return fir_f32(desc.coeffs, x)
    if desc.type == BIQUAD_Q31:
        return biquad_q31(desc.coeffs, desc.post_shift, x)
    if desc.type == BIQUAD_F32:
        return biquad_f32(desc.coeffs, x)
    raise ValueError(f"unknown filter type {desc.type}")


def xorshift32_q15(seed: int, n: int) -> np.ndarray:
    """The POST input generator in firmware/app/src/post.c."""
    x = seed & 0xFFFFFFFF
    out = np.empty(n, dtype=np.int16)
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = np.int16(np.uint16(x >> 16).view(np.int16))
    return out
