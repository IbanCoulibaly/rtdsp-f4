"""Host-side client for the board's data link (USART1 over a TCP socket in QEMU)."""

from __future__ import annotations

import select
import socket
import time
from collections import deque

import numpy as np

from . import BLOCK_MAX
from . import protocol as P
from .quantize import FilterDesc


class DeviceError(Exception):
    pass


class Device:
    def __init__(self, sock: socket.socket, timeout: float = 5.0):
        self._sock = sock
        self._sock.setblocking(False)
        self._reader = P.FrameReader()
        self._pending: deque[P.Frame] = deque()
        self._seq = 0
        self.timeout = timeout
        self.boots: list[P.DevInfo] = []
        self.stream: list[tuple[np.ndarray, np.ndarray]] = []

    # ------------------------------------------------------------ transport

    def send_raw(self, data: bytes) -> None:
        """Send arbitrary bytes (used by tests to inject corrupted frames)."""
        self._send_all(data)

    def _send_all(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            _, w, _ = select.select([], [self._sock], [], self.timeout)
            if not w:
                raise DeviceError("send timeout")
            n = self._sock.send(view)
            view = view[n:]

    def send(self, msg_type: int, payload: bytes = b"") -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFF
        self._send_all(P.encode_frame(msg_type, seq, payload))
        return seq

    def _pump(self, timeout: float) -> bool:
        r, _, _ = select.select([self._sock], [], [], timeout)
        if not r:
            return False
        data = self._sock.recv(65536)
        if not data:
            raise DeviceError("connection closed by emulator")
        for f in self._reader.feed(data):
            if f.type == P.MSG_BOOT:
                self.boots.append(P.DevInfo.parse(f.payload))
            elif f.type == P.MSG_STREAM:
                n = len(f.payload) // 4
                v = np.frombuffer(f.payload, dtype="<i2")
                self.stream.append((v[:n].copy(), v[n:].copy()))
            else:
                self._pending.append(f)
        return True

    def wait_for(self, pred, timeout: float | None = None) -> P.Frame:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            for i, f in enumerate(self._pending):
                if pred(f):
                    del self._pending[i]
                    return f
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceError("timeout waiting for response")
            self._pump(remaining)

    def poll(self, duration: float) -> None:
        end = time.monotonic() + duration
        while (left := end - time.monotonic()) > 0:
            self._pump(left)

    def _request(self, msg_type: int, payload: bytes, reply_type: int) -> P.Frame:
        seq = self.send(msg_type, payload)
        f = self.wait_for(lambda fr: fr.seq == seq and fr.type in (reply_type, P.MSG_ACK))
        if f.type == P.MSG_ACK and reply_type != P.MSG_ACK:
            raise DeviceError(f"{msg_type:#x} rejected: {P.STATUS_NAMES.get(f.payload[1], f.payload[1])}")
        return f

    def _ack(self, msg_type: int, payload: bytes = b"") -> int:
        f = self._request(msg_type, payload, P.MSG_ACK)
        if f.payload[0] != msg_type:
            raise DeviceError("ACK for a different message")
        return f.payload[1]

    # ------------------------------------------------------------- commands

    def wait_boot(self, timeout: float = 10.0) -> P.DevInfo:
        n = len(self.boots)
        deadline = time.monotonic() + timeout
        while len(self.boots) == n:
            if time.monotonic() > deadline:
                raise DeviceError("no BOOT frame")
            self._pump(0.05)
        return self.boots[-1]

    def ping(self) -> P.DevInfo:
        return P.DevInfo.parse(self._request(P.MSG_PING, b"", P.MSG_PONG).payload)

    def stats(self) -> P.Stats:
        return P.Stats.parse(self._request(P.MSG_GET_STATS, b"", P.MSG_STATS).payload)

    def set_filter(self, desc: FilterDesc | bytes, check: bool = True) -> int:
        raw = desc if isinstance(desc, bytes) else desc.to_bytes()
        status = self._ack(P.MSG_SET_FILTER, raw)
        if check and status != P.ST_OK:
            raise DeviceError(f"SET_FILTER rejected: {P.STATUS_NAMES.get(status, status)}")
        return status

    def reset_state(self) -> None:
        if self._ack(P.MSG_RESET_STATE) != P.ST_OK:
            raise DeviceError("RESET_STATE failed")

    def set_mode_host(self) -> None:
        if self._ack(P.MSG_SET_MODE, P.set_mode_payload(P.MODE_HOST_STREAM)) != P.ST_OK:
            raise DeviceError("SET_MODE failed")

    def set_mode_siggen(self, f1_hz: int, a1: float, f2_hz: int, a2: float, noise: float) -> None:
        payload = P.set_mode_payload(P.MODE_SIGGEN, f1_hz, a1, f2_hz, a2, noise)
        if self._ack(P.MSG_SET_MODE, payload) != P.ST_OK:
            raise DeviceError("SET_MODE failed")

    def inject_fault(self, kind: int) -> None:
        self.send(P.MSG_DEBUG_FAULT, bytes([kind]))

    def process(self, x: np.ndarray, block: int = BLOCK_MAX, window: int = 4) -> np.ndarray:
        """Stream x through the board's active filter.

        Sliding-window flow control: at most `window` PROCESS frames are in
        flight, so the board's 8-block pool and UART buffers are never
        overrun. Results must come back in order (FIFO pipeline).
        """
        x = np.asarray(x, dtype=np.int16)
        chunks = [x[i:i + block] for i in range(0, len(x), block)]
        out: list[np.ndarray] = []
        inflight: deque[int] = deque()
        nxt = 0
        while len(out) < len(chunks):
            while nxt < len(chunks) and len(inflight) < window:
                inflight.append(self.send(P.MSG_PROCESS, chunks[nxt].astype("<i2").tobytes()))
                nxt += 1
            seq = inflight.popleft()
            f = self.wait_for(lambda fr: fr.seq == seq and fr.type in (P.MSG_PROCESS_RESULT, P.MSG_ACK))
            if f.type == P.MSG_ACK:
                raise DeviceError(f"PROCESS rejected: {P.STATUS_NAMES.get(f.payload[1], f.payload[1])}")
            out.append(np.frombuffer(f.payload, dtype="<i2").astype(np.int16))
        return np.concatenate(out) if out else np.zeros(0, dtype=np.int16)

    def capture_stream(self, n_samples: int, timeout: float = 20.0) -> tuple[np.ndarray, np.ndarray]:
        deadline = time.monotonic() + timeout
        while sum(len(i) for i, _ in self.stream) < n_samples:
            if time.monotonic() > deadline:
                raise DeviceError("stream capture timeout")
            self._pump(0.05)
        xin = np.concatenate([i for i, _ in self.stream])[:n_samples]
        xout = np.concatenate([o for _, o in self.stream])[:n_samples]
        return xin, xout

    def close(self) -> None:
        self._sock.close()
