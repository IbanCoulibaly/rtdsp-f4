"""Coefficient quantisation and the wire descriptor.

This is where float designs become what the Cortex-M4 actually executes.
See docs/DSP_MATH.md section 3 (Q formats), 5.3 (post-shift) and 5.4 (scaling).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np
from scipy import signal

# Filter types: must match libs/dsp/include/dsp/filter_engine.h (checked by test_contract.py).
BYPASS, FIR_Q15, FIR_F32, BIQUAD_Q31, BIQUAD_F32 = 0, 1, 2, 3, 4
TYPE_NAMES = {BYPASS: "bypass", FIR_Q15: "fir_q15", FIR_F32: "fir_f32",
              BIQUAD_Q31: "biquad_q31", BIQUAD_F32: "biquad_f32"}
MAX_TAPS = 128
MAX_STAGES = 8
MAX_POST_SHIFT = 7


@dataclass(frozen=True)
class FilterDesc:
    """A quantised filter exactly as it travels to (and runs on) the board."""

    type: int
    order: int
    coeffs: np.ndarray  # int16 (FIR_Q15), int32 (n,5) (BIQUAD_Q31), float32 otherwise
    post_shift: int = 0
    label: str = ""

    def to_bytes(self) -> bytes:
        hdr = struct.pack("<BBH", self.type, self.post_shift, self.order)
        if self.type == BYPASS:
            return hdr
        dtype = {FIR_Q15: "<i2", FIR_F32: "<f4", BIQUAD_Q31: "<i4", BIQUAD_F32: "<f4"}[self.type]
        return hdr + np.asarray(self.coeffs).astype(dtype).ravel().tobytes()

    @property
    def name(self) -> str:
        return self.label or TYPE_NAMES[self.type]


# --------------------------------------------------------------------------- FIR

def fir_to_q15(h: np.ndarray) -> np.ndarray:
    """h -> round(h * 2^15), saturated to int16. Q15 cannot hold +1.0, so a
    coefficient of exactly 1.0 becomes 32767 (error 2^-15)."""
    q = np.round(np.asarray(h, dtype=np.float64) * 32768.0)
    if np.any(np.abs(h) > 1.0):
        raise ValueError("FIR coefficient magnitude > 1 is not representable in Q15")
    return np.clip(q, -32768, 32767).astype(np.int16)


def fir_q15(h: np.ndarray, label: str = "") -> FilterDesc:
    if not 1 <= len(h) <= MAX_TAPS:
        raise ValueError("tap count out of range")
    return FilterDesc(FIR_Q15, len(h), fir_to_q15(h), label=label)


def fir_f32(h: np.ndarray, label: str = "") -> FilterDesc:
    return FilterDesc(FIR_F32, len(h), np.asarray(h, dtype=np.float32), label=label)


def fir_l1_gain(h: np.ndarray) -> float:
    """Worst-case gain: max|y| <= sum|h| * max|x|. If > 1 the output can clip."""
    return float(np.sum(np.abs(h)))


# ------------------------------------------------------------------------- biquad

def sos_to_kernel(sos: np.ndarray) -> np.ndarray:
    """SOS rows [b0 b1 b2 1 a1 a2] -> kernel rows [b0 b1 b2 -a1 -a2] (float64)."""
    sos = np.atleast_2d(np.asarray(sos, dtype=np.float64))
    if not np.allclose(sos[:, 3], 1.0):
        raise ValueError("SOS must be normalised so a0 == 1")
    return np.column_stack([sos[:, 0], sos[:, 1], sos[:, 2], -sos[:, 4], -sos[:, 5]])


def choose_post_shift(kernel: np.ndarray) -> int:
    """Smallest shift s such that, for every section,
        max|c| < 2^s         (each coefficient fits Q(31-s)), and
        sum|c| < 2^(1+s)     (the 64-bit DF1 accumulator cannot overflow:
                              |acc| <= sum|c_int| * 2^31 < 2^(31-s) * 2^(1+s) * 2^31 = 2^63)
    """
    for s in range(MAX_POST_SHIFT + 1):
        if np.all(np.abs(kernel) < 2.0 ** s) and np.all(np.sum(np.abs(kernel), axis=1) < 2.0 ** (1 + s)):
            return s
    raise ValueError("coefficients too large for any supported post_shift")


def biquad_q31(sos: np.ndarray, post_shift: int | None = None, label: str = "") -> FilterDesc:
    k = sos_to_kernel(sos)
    if len(k) > MAX_STAGES:
        raise ValueError("too many sections")
    ps = choose_post_shift(k) if post_shift is None else post_shift
    q = np.round(k * 2.0 ** (31 - ps))
    q = np.clip(q, -(2 ** 31), 2 ** 31 - 1).astype(np.int64)
    return FilterDesc(BIQUAD_Q31, len(k), q.astype(np.int32), post_shift=ps, label=label)


def biquad_f32(sos: np.ndarray, label: str = "") -> FilterDesc:
    k = sos_to_kernel(sos)
    if len(k) > MAX_STAGES:
        raise ValueError("too many sections")
    return FilterDesc(BIQUAD_F32, len(k), k.astype(np.float32), label=label)


def bypass() -> FilterDesc:
    return FilterDesc(BYPASS, 0, np.zeros(0, dtype=np.int16), label="bypass")


def scale_sos_linf(sos: np.ndarray, headroom: float = 1.0, n_freq: int = 8192) -> np.ndarray:
    """L-infinity scaling for fixed-point cascades.

    For each internal node k the cumulative response G_k = H_1 ... H_k is
    computed and section k's numerator is scaled so max_w |G_k(e^jw)| = headroom.
    A full-scale sinusoid at any frequency then cannot overflow any
    intermediate node. The last section absorbs the remaining gain so the
    overall response is unchanged.
    """
    sos = np.array(sos, dtype=np.float64, copy=True)
    _, h_total = signal.sosfreqz(sos, worN=n_freq)
    cumulative = np.ones(n_freq, dtype=complex)
    applied = 1.0
    for k in range(len(sos) - 1):
        _, hk = signal.sosfreqz(sos[k:k + 1], worN=n_freq)
        cumulative *= hk
        peak = np.max(np.abs(cumulative))
        g = headroom / peak
        sos[k, :3] *= g
        cumulative *= g
        applied *= g
    sos[-1, :3] /= applied
    _, h_new = signal.sosfreqz(sos, worN=n_freq)
    assert np.allclose(h_new, h_total, atol=1e-9)
    return sos


def quantised_sos(desc: FilterDesc) -> np.ndarray:
    """Back from what the board runs to SOS form (for response/pole analysis)."""
    if desc.type == BIQUAD_Q31:
        k = desc.coeffs.astype(np.float64) / 2.0 ** (31 - desc.post_shift)
    elif desc.type == BIQUAD_F32:
        k = desc.coeffs.astype(np.float64)
    else:
        raise ValueError("not a biquad descriptor")
    return np.column_stack([k[:, 0], k[:, 1], k[:, 2], np.ones(len(k)), -k[:, 3], -k[:, 4]])


def quantised_fir(desc: FilterDesc) -> np.ndarray:
    if desc.type == FIR_Q15:
        return desc.coeffs.astype(np.float64) / 32768.0
    if desc.type == FIR_F32:
        return desc.coeffs.astype(np.float64)
    raise ValueError("not an FIR descriptor")
