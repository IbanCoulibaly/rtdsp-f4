"""Software-in-the-loop: the firmware's C filter engine, compiled for the host,
against the bit-exact Python models and against float64 scipy."""

import numpy as np
import pytest
from scipy import signal

from rtdsp import analysis, fixedpoint
from rtdsp import quantize as Q
from rtdsp.sil import SilEngine


@pytest.fixture(params=range(5), ids=lambda i: ["fir_q15", "fir_f32", "ellip_q31", "cheby_f32", "notch_q31"][i])
def desc(request, suite):
    return suite[request.param]


def test_c_engine_is_bit_exact_with_python_model(sil_lib, desc):
    x = analysis.test_signal(3000, seed=7)
    y_c = SilEngine(sil_lib, desc).process(x)
    y_py = fixedpoint.engine(desc, x)
    assert np.array_equal(y_c, y_py)


def test_block_size_does_not_change_output(sil_lib, desc, rng):
    x = rng.integers(-12000, 12000, 1500).astype(np.int16)
    ref = SilEngine(sil_lib, desc).process(x)
    for block in (1, 7, 32, 64, 100):
        assert np.array_equal(SilEngine(sil_lib, desc).process(x, block=block), ref)


def test_reset_clears_state(sil_lib, desc, rng):
    e = SilEngine(sil_lib, desc)
    x = rng.integers(-12000, 12000, 500).astype(np.int16)
    first = e.process(x)
    e.reset()
    assert np.array_equal(e.process(x), first)


def test_close_to_ideal_float64_filter(sil_lib, desc):
    """Quantisation noise only: SQNR vs an ideal float64 implementation."""
    x = analysis.test_signal(8000, seed=3)
    y = SilEngine(sil_lib, desc).process(x).astype(float)
    xf = x.astype(float)
    if desc.type in (Q.FIR_Q15, Q.FIR_F32):
        ideal = signal.lfilter(Q.quantised_fir(desc), 1.0, xf)
    else:
        ideal = signal.sosfilt(Q.quantised_sos(desc), xf)
    assert analysis.sqnr_db(ideal, y) > 55.0  # 16-bit output: ~6 dB/bit, minus filter noise gain


def test_saturates_instead_of_wrapping(sil_lib):
    loud = Q.fir_q15(np.array([0.99, 0.99]))  # gain ~2
    x = np.full(64, 30000, dtype=np.int16)
    y = SilEngine(sil_lib, loud).process(x)
    assert y[1:].min() == 32767


@pytest.mark.parametrize("raw,status", [
    (bytes([9, 0, 1, 0, 0, 0]), 1),                        # unknown type
    (bytes([Q.FIR_Q15, 0, 0, 0]), 2),                      # zero taps
    (bytes([Q.FIR_Q15, 0, 2, 0, 1, 0]), 3),                # length mismatch
    (bytes([Q.BIQUAD_Q31, 9, 1, 0]) + bytes(20), 4),       # post_shift too big
])
def test_engine_rejects_bad_descriptors(sil_lib, raw, status):
    assert SilEngine(sil_lib).configure(raw) == status


def test_engine_rejects_unstable_biquad(sil_lib):
    unstable = np.array([[1.0, 0.0, 0.0, 1.0, -1.9, 0.95 * 1.1]])  # |a2| = 1.045 >= 1
    desc = Q.FilterDesc(Q.BIQUAD_F32, 1, Q.sos_to_kernel(unstable).astype(np.float32))
    assert SilEngine(sil_lib).configure(desc) == 5


def test_firmware_golden_crcs_match_model():
    """The constants baked into fw_tables.h are what the model produces today."""
    import gen_firmware_tables as g  # noqa: PLC0415 (python/tools is on pytest.ini pythonpath)
    x = fixedpoint.xorshift32_q15(g.POST_SEED, g.POST_LEN)
    header = (g.OUT_DIR / "fw_tables.h").read_text()
    assert f"POST_FIR_Q15_CRC 0x{g.golden_crc(g.default_filter(), x):04X}u" in header
    assert f"POST_BIQUAD_Q31_CRC 0x{g.golden_crc(g.post_biquad(), x):04X}u" in header
