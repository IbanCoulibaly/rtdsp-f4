"""The reference filter set used by the tests, the demo and the visualiser:
one design per kernel type, covering FIR/IIR, fixed/float, and the classic
prototypes. Each spec keeps its design parameters so tools can explain it."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import design
from . import quantize as Q


@dataclass(frozen=True)
class FilterSpec:
    desc: Q.FilterDesc            # what runs on the board
    ideal: np.ndarray             # float64 taps (FIR) or SOS (IIR) before quantisation
    family: str                   # 'kaiser' | 'hamming' | 'ellip' | 'cheby1' | 'notch'
    params: dict = field(default_factory=dict)
    tones: tuple = ()             # showcase input: frequencies (Hz) straddling the band edges


def demo_specs() -> list[FilterSpec]:
    kaiser = design.kaiser_fir(5000, 2000, 60)
    bandpass = design.windowed_fir(63, [2000, 8000], pass_zero="bandpass")
    ellip = design.iir_sos("ellip", 6, 3000, rp=0.5, rs=60)
    cheby = design.iir_sos("cheby1", 4, 1000, btype="highpass", rp=1.0)
    notch = design.notch_sos(1000, 10)
    return [
        FilterSpec(Q.fir_q15(kaiser, label="FIR Q15 low-pass 5 kHz (Kaiser, 60 dB)"), kaiser, "kaiser",
                   {"fc": 5000, "transition": 2000, "atten": 60}, (1000, 9000)),
        FilterSpec(Q.fir_f32(bandpass, label="FIR f32 band-pass 2-8 kHz (Hamming, 63 taps)"), bandpass,
                   "hamming", {"f1": 2000, "f2": 8000}, (500, 4000, 14000)),
        FilterSpec(Q.biquad_q31(Q.scale_sos_linf(ellip), label="IIR Q31 elliptic LP 3 kHz (6th order)"), ellip,
                   "ellip", {"fc": 3000, "order": 6, "rp": 0.5, "rs": 60}, (800, 6000)),
        FilterSpec(Q.biquad_f32(cheby, label="IIR f32 Chebyshev-I HP 1 kHz (4th order)"), cheby, "cheby1",
                   {"fc": 1000, "order": 4, "rp": 1.0}, (200, 4000)),
        FilterSpec(Q.biquad_q31(notch, label="IIR Q31 notch 1 kHz (Q = 10)"), notch, "notch",
                   {"f0": 1000, "q": 10}, (1000, 3000)),
    ]


def demo_filters() -> list[Q.FilterDesc]:
    return [s.desc for s in demo_specs()]
