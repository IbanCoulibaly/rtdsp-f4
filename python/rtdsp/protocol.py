"""Wire protocol mirror of libs/link (COBS + CRC-16/CCITT-FALSE framing).

Constants here are checked against libs/link/include/link/protocol_defs.h
by python/tests/test_contract.py, so the two sides cannot drift apart.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

PROTOCOL_VERSION = 1
MAX_PAYLOAD = 600

MSG_PING = 0x01
MSG_SET_FILTER = 0x02
MSG_PROCESS = 0x03
MSG_GET_STATS = 0x04
MSG_RESET_STATE = 0x05
MSG_SET_MODE = 0x06
MSG_DEBUG_FAULT = 0x7E
MSG_PONG = 0x81
MSG_ACK = 0x82
MSG_PROCESS_RESULT = 0x83
MSG_STATS = 0x84
MSG_STREAM = 0x85
MSG_BOOT = 0x87

ST_OK = 0
ST_ERR_FILTER_TYPE = 1
ST_ERR_FILTER_ORDER = 2
ST_ERR_FILTER_LEN = 3
ST_ERR_FILTER_PARAM = 4
ST_ERR_UNSTABLE = 5
ST_ERR_BUSY = 6
ST_ERR_MODE = 7
ST_ERR_UNKNOWN_MSG = 8
ST_ERR_LEN = 9
STATUS_NAMES = {v: k for k, v in list(globals().items()) if k.startswith("ST_")}

MODE_HOST_STREAM = 0
MODE_SIGGEN = 1

DBG_FAULT_UDF = 1
DBG_FAULT_ASSERT = 2

RESET_POWER_ON = 0
RESET_SOFTWARE = 1
RESET_WATCHDOG = 2
RESET_FAULT = 3
RESET_ASSERT = 4
RESET_STACK_OVERFLOW = 5
RESET_TASK_HANG = 6
RESET_PIN = 7

POST_FAIL_CRC_ENGINE = 0x01
POST_FAIL_FIR_Q15 = 0x02
POST_FAIL_BIQUAD_Q31 = 0x04
POST_FAIL_FIR_F32 = 0x08
POST_FAIL_CLOCK = 0x10

DEVINFO_FMT = "<4BI3H2B4I2B16s"
STATS_FMT = "<15I5H4B"
DEVINFO_LEN = struct.calcsize(DEVINFO_FMT)
STATS_LEN = struct.calcsize(STATS_FMT)
TASK_NAMES = ("dsp", "link_rx", "link_tx", "supervisor", "log")


# --------------------------------------------------------------------- CRC / COBS

def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """Same algorithm as libs/link/src/cobs.c, byte for byte."""
    out = bytearray([0])
    code_idx, code = 0, 1
    for b in data:
        if b == 0:
            out[code_idx] = code
            code_idx, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx, code = len(out), 1
                out.append(0)
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            raise ValueError("malformed COBS block")
        block = data[i:i + code - 1]
        if 0 in block:
            raise ValueError("zero inside COBS block")
        out += block
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = bytes([msg_type, seq & 0xFF]) + payload
    return cobs_encode(body + struct.pack("<H", crc16_ccitt(body))) + b"\x00"


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes


class FrameError(Exception):
    pass


def decode_frame(stuffed: bytes) -> Frame:
    raw = cobs_decode(stuffed)
    if len(raw) < 4:
        raise FrameError("short frame")
    body, crc = raw[:-2], struct.unpack("<H", raw[-2:])[0]
    if crc16_ccitt(body) != crc:
        raise FrameError("CRC mismatch")
    return Frame(body[0], body[1], body[2:])


class FrameReader:
    """Incremental de-framer: feed arbitrary byte chunks, get whole frames."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.errors = 0

    def feed(self, data: bytes) -> list[Frame]:
        frames = []
        self._buf += data
        while True:
            try:
                end = self._buf.index(0)
            except ValueError:
                return frames
            chunk = bytes(self._buf[:end])
            del self._buf[:end + 1]
            if not chunk:
                continue
            try:
                frames.append(decode_frame(chunk))
            except (ValueError, FrameError):
                self.errors += 1


# ------------------------------------------------------------------- payloads

@dataclass
class DevInfo:
    protocol_version: int
    fw_version: str
    fs_hz: int
    block_size: int
    block_max: int
    max_taps: int
    max_stages: int
    reset_reason: int
    fault_pc: int
    fault_lr: int
    fault_cfsr: int
    fault_info: int
    post_result: int
    target: str
    build: str

    @classmethod
    def parse(cls, p: bytes) -> "DevInfo":
        v = struct.unpack(DEVINFO_FMT, p)
        return cls(v[0], f"{v[1]}.{v[2]}.{v[3]}", v[4], v[5], v[6], v[7], v[8], v[9],
                   v[10], v[11], v[12], v[13], v[14], "qemu" if v[15] == 0 else "hw",
                   v[16].rstrip(b"\x00").decode())


@dataclass
class Stats:
    uptime_ms: int
    blocks_processed: int
    samples_processed: int
    cycles_last_block: int
    cycles_max_block: int
    cycles_per_sample_q8: int
    cycles_budget_per_sample: int
    rx_frames_ok: int
    rx_crc_errors: int
    rx_framing_errors: int
    rx_uart_dropped: int
    pool_exhausted: int
    tx_dropped: int
    siggen_overruns: int
    filter_swaps: int
    stack_free_words: dict
    active_filter_type: int
    cpu_load_pct: int
    mode: int

    @classmethod
    def parse(cls, p: bytes) -> "Stats":
        v = struct.unpack(STATS_FMT, p)
        return cls(*v[:15], dict(zip(TASK_NAMES, v[15:20])), v[20], v[21], v[22])

    @property
    def cycles_per_sample(self) -> float:
        return self.cycles_per_sample_q8 / 256.0

    @property
    def dsp_utilisation(self) -> float:
        """Fraction of the real-time budget (f_cpu / fs cycles per sample) used by filtering."""
        return self.cycles_per_sample / self.cycles_budget_per_sample


def set_mode_payload(mode: int, f1_hz: int = 0, a1: float = 0.0, f2_hz: int = 0,
                     a2: float = 0.0, noise: float = 0.0) -> bytes:
    q = lambda v: int(round(max(-1.0, min(v, 32767 / 32768)) * 32768))  # noqa: E731
    if mode == MODE_HOST_STREAM:
        return bytes([mode])
    return struct.pack("<BHhHhh", mode, f1_hz, q(a1), f2_hz, q(a2), q(noise))
