"""Processor-in-the-loop: the real firmware image running on QEMU's STM32F405.

Every test here talks to the board only through the wire protocol, exactly
as a production test station would talk to a board on a bench.
"""

import time

import numpy as np
import pytest

from rtdsp import analysis, design, fixedpoint
from rtdsp import protocol as P
from rtdsp import quantize as Q
from rtdsp.device import DeviceError

pytestmark = pytest.mark.emulator


def test_boot_report_and_post(board):
    info = board.boot_info
    assert info.post_result == 0, f"POST failed: {info.post_result:#x}"
    assert info.reset_reason == P.RESET_POWER_ON
    assert (info.target, info.fs_hz, info.block_max) == ("qemu", 48000, 64)
    assert "POST PASS" in board.qemu.console()


def test_default_filter_is_active_at_boot(board):
    import gen_firmware_tables as g  # noqa: PLC0415
    x = analysis.test_signal(1024, seed=11)
    board.reset_state()
    assert np.array_equal(board.process(x), fixedpoint.engine(g.default_filter(), x))


@pytest.mark.parametrize("idx", range(5), ids=["fir_q15", "fir_f32", "ellip_q31", "cheby_f32", "notch_q31"])
def test_target_output_bit_exact(board, suite, idx):
    desc = suite[idx]
    board.set_filter(desc)
    x = analysis.test_signal(2048, seed=idx)
    y = board.process(x)
    assert np.array_equal(y, fixedpoint.engine(desc, x)), "target diverged from bit-exact model"
    assert board.stats().active_filter_type == desc.type


def test_notch_rejects_tone_on_target(board):
    board.set_filter(Q.biquad_q31(design.notch_sos(1000, 10)))
    x = analysis.tones(9600, [1000, 3000], 0.4)
    y = board.process(x)
    assert analysis.tone_gain_db(x, y, 1000) < -40
    assert abs(analysis.tone_gain_db(x, y, 3000)) < 0.5


def test_swap_happens_exactly_between_blocks(board, suite):
    """PROCESS, PROCESS, SET_FILTER, PROCESS... pipelined without waiting:
    the DSP queue carries data and commands in order, so blocks sent before
    the SET_FILTER use filter A and blocks after it use a fresh filter B."""
    a, b = suite[0], suite[2]
    board.set_filter(a)
    x1 = analysis.test_signal(4 * 64, seed=21)
    x2 = analysis.test_signal(4 * 64, seed=22)
    seqs1 = [board.send(P.MSG_PROCESS, c.astype("<i2").tobytes()) for c in x1.reshape(4, 64)]
    swap = board.send(P.MSG_SET_FILTER, b.to_bytes())
    seqs2 = [board.send(P.MSG_PROCESS, c.astype("<i2").tobytes()) for c in x2.reshape(4, 64)]

    def result(seq):
        f = board.wait_for(lambda fr: fr.seq == seq)
        assert f.type == P.MSG_PROCESS_RESULT
        return np.frombuffer(f.payload, "<i2")

    y1 = np.concatenate([result(s) for s in seqs1])
    ack = board.wait_for(lambda fr: fr.seq == swap)
    assert ack.type == P.MSG_ACK and ack.payload[1] == P.ST_OK
    y2 = np.concatenate([result(s) for s in seqs2])
    assert np.array_equal(y1, fixedpoint.engine(a, x1))
    assert np.array_equal(y2, fixedpoint.engine(b, x2))


def test_unstable_filter_rejected_and_previous_kept(board, suite):
    board.set_filter(suite[0])
    bad = Q.FilterDesc(Q.BIQUAD_F32, 1, np.array([[1, 0, 0, 1.2, 0]], dtype=np.float32))  # pole at z = 1.2
    assert board.set_filter(bad, check=False) == P.ST_ERR_UNSTABLE
    assert board.set_filter(b"\x01\x00\x05\x00\x00", check=False) == P.ST_ERR_FILTER_LEN
    board.reset_state()
    x = analysis.test_signal(512, seed=5)
    assert np.array_equal(board.process(x), fixedpoint.engine(suite[0], x))


def test_link_survives_corruption(board):
    before = board.stats()
    frame = bytearray(P.encode_frame(P.MSG_PING, 99))
    frame[2] ^= 0x40                                       # flip a bit: CRC must catch it
    board.send_raw(bytes(frame))
    board.send_raw(b"\x05\x01\x02\x00")                    # truncated COBS block
    board.send_raw(bytes(range(1, 256)) * 6 + b"\x00")     # oversize frame (> FRAME_WIRE_MAX)
    assert board.ping().fw_version == "1.0.0"
    after = board.stats()
    assert after.rx_crc_errors == before.rx_crc_errors + 1
    assert after.rx_framing_errors >= before.rx_framing_errors + 2
    with pytest.raises(DeviceError):
        board.wait_for(lambda f: f.seq == 99, timeout=0.3)  # the corrupted PING got no answer


def test_unknown_message_and_bad_lengths_are_nacked(board):
    seq = board.send(0x55)
    f = board.wait_for(lambda fr: fr.seq == seq)
    assert (f.type, f.payload[1]) == (P.MSG_ACK, P.ST_ERR_UNKNOWN_MSG)
    seq = board.send(P.MSG_PROCESS, b"\x01\x02\x03")       # odd byte count
    f = board.wait_for(lambda fr: fr.seq == seq)
    assert f.payload[1] == P.ST_ERR_LEN


def test_siggen_isr_pipeline(board, suite):
    """Sample clock ISR -> pool -> DSP task -> link_tx, with no host pacing."""
    notch = suite[4]
    board.set_filter(notch)
    board.stream.clear()
    s0 = board.stats()
    t0 = time.monotonic()
    board.set_mode_siggen(1000, 0.4, 3000, 0.4, 0.02)
    xin, xout = board.capture_stream(24000)
    elapsed = time.monotonic() - t0
    seq = board.send(P.MSG_PROCESS, b"\x00\x00")
    assert board.wait_for(lambda fr: fr.seq == seq).payload[1] == P.ST_ERR_MODE
    board.set_mode_host()
    s1 = board.stats()

    assert s1.tx_dropped == s0.tx_dropped
    assert np.array_equal(xout, fixedpoint.engine(notch, xin))
    assert analysis.tone_gain_db(xin, xout, 1000) < -40
    assert abs(analysis.tone_gain_db(xin, xout, 3000)) < 0.5
    rate = len(xin) / elapsed
    assert 0.3 * 48000 < rate < 3.0 * 48000, f"sample clock ran at {rate:.0f} Hz"


def test_health_metrics(board):
    board.set_filter(Q.fir_q15(design.kaiser_fir(5000, 2000, 60)))
    board.process(analysis.test_signal(2048))
    board.poll(1.1)  # let the supervisor run twice
    s = board.stats()
    assert s.blocks_processed > 0 and s.cycles_last_block > 0
    assert 0 < s.dsp_utilisation < 1.0
    assert s.cycles_budget_per_sample == 168_000_000 // 48_000
    assert min(s.stack_free_words.values()) >= 64, s.stack_free_words
    assert s.cpu_load_pct < 100
    assert s.pool_exhausted == 0 and s.rx_uart_dropped == 0
