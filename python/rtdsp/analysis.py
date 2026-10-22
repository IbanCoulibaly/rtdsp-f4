"""Measurements used by the tests and the demo report."""

from __future__ import annotations

import numpy as np
from scipy import signal

from . import FS_HZ
from .quantize import FIR_F32, FIR_Q15, FilterDesc, quantised_fir, quantised_sos


def response(desc: FilterDesc, fs: float = FS_HZ, n: int = 4096):
    """Frequency response H(e^jw) of the coefficients the board actually runs."""
    if desc.type in (FIR_Q15, FIR_F32):
        return signal.freqz(quantised_fir(desc), worN=n, fs=fs)
    return signal.sosfreqz(quantised_sos(desc), worN=n, fs=fs)


def mag_db(h: np.ndarray) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))


def pole_radii(sos: np.ndarray) -> np.ndarray:
    """|p| of every pole. Stability <=> all < 1; margin = 1 - max."""
    _, p, _ = signal.sos2zpk(sos)
    return np.abs(p)


def tone_gain_db(x: np.ndarray, y: np.ndarray, f: float, fs: float = FS_HZ) -> float:
    """Gain at one frequency via correlation with exp(-j w n) (single-bin DFT),
    on the second half of the record to skip the start-up transient."""
    n = np.arange(len(x))
    half = len(x) // 2
    ref = np.exp(-2j * np.pi * f / fs * n[half:])
    gx = np.abs(np.dot(x[half:].astype(float), ref))
    gy = np.abs(np.dot(y[half:].astype(float), ref))
    return 20.0 * np.log10(max(gy, 1e-12) / max(gx, 1e-12))


def sqnr_db(reference: np.ndarray, test: np.ndarray) -> float:
    """Signal-to-quantisation-noise ratio of `test` against `reference`."""
    reference = np.asarray(reference, dtype=float)
    err = np.asarray(test, dtype=float) - reference
    p_err = np.sum(err ** 2)
    return float("inf") if p_err == 0 else 10.0 * np.log10(np.sum(reference ** 2) / p_err)


def test_signal(n: int, fs: float = FS_HZ, seed: int = 1, level: float = 0.25) -> np.ndarray:
    """Log chirp 20 Hz..fs/2 plus white noise, as Q15. `level` is the peak amplitude per component."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / fs
    chirp = signal.chirp(t, f0=20.0, t1=t[-1], f1=0.45 * fs, method="logarithmic")
    noise = rng.uniform(-1.0, 1.0, n)
    x = level * chirp + level * noise
    return np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16)


def tones(n: int, freqs, amp: float, fs: float = FS_HZ) -> np.ndarray:
    t = np.arange(n) / fs
    x = sum(amp * np.sin(2 * np.pi * f * t) for f in freqs)
    return np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16)
