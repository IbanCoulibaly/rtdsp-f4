"""Design + quantisation: the coefficients that reach the board still meet spec."""

import numpy as np
import pytest
from scipy import signal

from rtdsp import analysis, design
from rtdsp import quantize as Q


def test_kaiser_fir_meets_attenuation_after_q15():
    h = design.kaiser_fir(5000, 2000, 60)
    desc = Q.fir_q15(h)
    f, H = analysis.response(desc)
    db = analysis.mag_db(H)
    assert np.all(np.abs(db[f <= 4000]) < 0.1)           # pass band flat to 0.1 dB
    assert np.max(db[f >= 6000]) < -57                    # Q15 costs at most ~3 dB of the 60 dB
    # Linear phase: symmetric taps survive rounding exactly.
    assert np.array_equal(desc.coeffs, desc.coeffs[::-1])


def test_post_shift_rule_prevents_accumulator_overflow():
    for sos in (design.butter_sos(8, 200), design.iir_sos("ellip", 8, 15000), design.notch_sos(50, 30)):
        desc = Q.biquad_q31(sos)
        k = desc.coeffs.astype(np.int64)
        worst = np.sum(np.abs(k), axis=1) * 2 ** 31   # every |x|, |y| at full scale
        assert np.all(worst < 2 ** 63)
        assert np.all(np.abs(Q.sos_to_kernel(sos)) < 2.0 ** desc.post_shift)


def test_q31_quantisation_preserves_poles_and_stability():
    sos = design.iir_sos("ellip", 6, 3000, rp=0.5, rs=60)
    qs = Q.quantised_sos(Q.biquad_q31(sos))
    r_ideal, r_q = analysis.pole_radii(sos), analysis.pole_radii(qs)
    assert np.max(r_q) < 1.0
    assert np.max(np.abs(np.sort(r_ideal) - np.sort(r_q))) < 1e-7


def test_linf_scaling_keeps_response_and_bounds_internal_gain():
    sos = design.iir_sos("cheby1", 6, 2000, rp=1.0)
    scaled = Q.scale_sos_linf(sos)
    _, h0 = signal.sosfreqz(sos, 4096)
    _, h1 = signal.sosfreqz(scaled, 4096)
    assert np.allclose(h0, h1, atol=1e-9)
    for k in range(1, len(scaled)):
        _, hk = signal.sosfreqz(scaled[:k], 4096)
        assert np.max(np.abs(hk)) <= 1.0 + 1e-9


def test_notch_depth_and_bandwidth():
    desc = Q.biquad_q31(design.notch_sos(1000, 10))
    f, H = analysis.response(desc, n=48000)
    db = analysis.mag_db(H)
    assert db[np.argmin(np.abs(f - 1000))] < -60
    assert abs(db[np.argmin(np.abs(f - 2000))]) < 0.5


def test_wire_descriptor_layout():
    d = Q.biquad_q31(design.butter_sos(2, 1000))
    raw = d.to_bytes()
    assert raw[0] == Q.BIQUAD_Q31 and raw[1] == d.post_shift
    assert int.from_bytes(raw[2:4], "little") == 1
    assert len(raw) == 4 + 5 * 4


def test_rejects_unrepresentable_designs():
    with pytest.raises(ValueError):
        Q.fir_q15(np.full(4, 1.5))
    with pytest.raises(ValueError):
        design.kaiser_fir(5000, 100, 80)  # would need far more than 128 taps
