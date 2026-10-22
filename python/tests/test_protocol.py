"""Framing: Python codec vs. the C codec compiled into the SIL library."""

import ctypes

import numpy as np
import pytest

from rtdsp import protocol as P


def test_crc_check_value():
    assert P.crc16_ccitt(b"123456789") == 0x29B1


@pytest.mark.parametrize("n", [0, 1, 253, 254, 255, 508, 600])
def test_cobs_roundtrip_edge_lengths(n, rng):
    for data in (bytes(n), bytes(rng.integers(1, 256, n, dtype=np.uint8)),
                 bytes(rng.integers(0, 256, n, dtype=np.uint8))):
        enc = P.cobs_encode(data)
        assert 0 not in enc
        assert len(enc) <= n + n // 254 + 1
        assert P.cobs_decode(enc) == data


def test_frame_reader_resyncs_after_garbage():
    good = P.encode_frame(P.MSG_PING, 7, b"abc")
    stream = b"\x13\x37garbage\x00" + good[:3] + b"\x00" + good + good
    r = P.FrameReader()
    frames = r.feed(stream[:5]) + r.feed(stream[5:])
    assert [f.seq for f in frames] == [7, 7]
    assert r.errors == 2


def test_c_and_python_build_identical_frames(sil_lib, rng):
    for _ in range(50):
        payload = bytes(rng.integers(0, 256, int(rng.integers(0, 600)), dtype=np.uint8))
        typ, seq = int(rng.integers(0, 256)), int(rng.integers(0, 256))
        buf = ctypes.create_string_buffer(700)
        n = sil_lib.sil_frame_build(typ, seq, payload, len(payload), buf, len(buf))
        assert buf.raw[:n] == P.encode_frame(typ, seq, payload)
        assert sil_lib.sil_crc16(payload, len(payload)) == P.crc16_ccitt(payload)
