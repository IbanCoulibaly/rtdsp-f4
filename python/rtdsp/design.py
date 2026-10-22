"""Filter design (floating point, float64). See docs/DSP_MATH.md sections 4-6.

All IIR designs are returned as second-order sections (SOS), shape (n, 6),
rows [b0, b1, b2, a0=1, a1, a2] -- the cascade form the firmware runs.
"""

from __future__ import annotations

import numpy as np
from scipy import signal

from . import FS_HZ


def iir_sos(kind: str, order: int, fc, fs: float = FS_HZ, btype: str = "lowpass",
            rp: float = 0.5, rs: float = 60.0) -> np.ndarray:
    """Classic analog prototype -> bilinear transform -> SOS.

    kind: 'butter' | 'cheby1' | 'cheby2' | 'ellip' | 'bessel'.
    scipy pre-warps the band edges (Omega = 2 fs tan(pi f / fs)) so the
    digital cut-off lands exactly at fc despite the bilinear frequency warping.
    """
    return signal.iirfilter(order, fc, rp=rp, rs=rs, btype=btype, ftype=kind,
                            output="sos", fs=fs)


def butter_sos(order: int, fc, fs: float = FS_HZ, btype: str = "lowpass") -> np.ndarray:
    return iir_sos("butter", order, fc, fs, btype)


def notch_sos(f0: float, q: float, fs: float = FS_HZ) -> np.ndarray:
    """RBJ "Audio EQ Cookbook" notch: zeros on the unit circle at +/-w0,
    poles at the same angle just inside it (radius set by Q).

        w0 = 2 pi f0 / fs,   alpha = sin(w0) / (2 Q)
        H(z) = (1 - 2cos(w0) z^-1 + z^-2) / ((1+alpha) - 2cos(w0) z^-1 + (1-alpha) z^-2)
    """
    w0 = 2.0 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2.0 * q)
    b = np.array([1.0, -2.0 * np.cos(w0), 1.0])
    a = np.array([1.0 + alpha, -2.0 * np.cos(w0), 1.0 - alpha])
    return np.hstack([b / a[0], a / a[0]])[None, :]


def kaiser_fir(fc, transition_hz: float, atten_db: float, fs: float = FS_HZ,
               pass_zero="lowpass", max_taps: int = 128) -> np.ndarray:
    """Windowed-sinc FIR with Kaiser's empirical order/beta formulas:

        N    ~ (A - 7.95) / (2.285 * dw) + 1,   dw = 2 pi * transition / fs
        beta = 0.1102 (A - 8.7)                      A > 50
             = 0.5842 (A - 21)^0.4 + 0.07886 (A - 21)  21 <= A <= 50
    The tap count is forced odd (type I linear phase, valid for any band type).
    """
    numtaps, beta = signal.kaiserord(atten_db, transition_hz / (0.5 * fs))
    numtaps |= 1
    if numtaps > max_taps:
        raise ValueError(f"spec needs {numtaps} taps; firmware limit is {max_taps}")
    return signal.firwin(numtaps, fc, window=("kaiser", beta), pass_zero=pass_zero, fs=fs)


def windowed_fir(numtaps: int, fc, fs: float = FS_HZ, window="hamming",
                 pass_zero="lowpass") -> np.ndarray:
    return signal.firwin(numtaps, fc, window=window, pass_zero=pass_zero, fs=fs)
