"""ctypes driver for build/host/librtdsp_sil.* (software-in-the-loop)."""

from __future__ import annotations

import ctypes
import sys
from pathlib import Path

import numpy as np

from . import REPO_ROOT
from .quantize import FilterDesc

_EXT = "dylib" if sys.platform == "darwin" else "so"
DEFAULT_LIB = REPO_ROOT / "build" / "host" / f"librtdsp_sil.{_EXT}"

_c_i16p = np.ctypeslib.ndpointer(dtype=np.int16, flags="C_CONTIGUOUS")
_c_u8p = ctypes.c_char_p


def load(path: Path = DEFAULT_LIB) -> ctypes.CDLL:
    if not path.exists():
        raise FileNotFoundError(f"{path} missing: run `make sil`")
    lib = ctypes.CDLL(str(path))
    lib.sil_engine_size.restype = ctypes.c_size_t
    lib.sil_engine_configure.argtypes = [ctypes.c_void_p, _c_u8p, ctypes.c_size_t]
    lib.sil_engine_configure.restype = ctypes.c_int
    lib.sil_engine_reset.argtypes = [ctypes.c_void_p]
    lib.sil_engine_process.argtypes = [ctypes.c_void_p, _c_i16p, _c_i16p, ctypes.c_size_t]
    lib.sil_crc16.argtypes = [_c_u8p, ctypes.c_size_t]
    lib.sil_crc16.restype = ctypes.c_uint16
    lib.sil_frame_build.argtypes = [ctypes.c_uint8, ctypes.c_uint8, _c_u8p, ctypes.c_size_t,
                                    ctypes.c_char_p, ctypes.c_size_t]
    lib.sil_frame_build.restype = ctypes.c_size_t
    return lib


class SilEngine:
    """One filter_engine_t living in host memory, driven exactly like the firmware drives it."""

    def __init__(self, lib: ctypes.CDLL, desc: FilterDesc | None = None):
        self._lib = lib
        words = (lib.sil_engine_size() + 7) // 8
        self._mem = (ctypes.c_uint64 * words)()  # 8-byte aligned storage
        if desc is not None:
            status = self.configure(desc)
            if status != 0:
                raise ValueError(f"engine rejected descriptor: status {status}")

    def configure(self, desc: FilterDesc | bytes) -> int:
        raw = desc if isinstance(desc, bytes) else desc.to_bytes()
        return self._lib.sil_engine_configure(self._mem, raw, len(raw))

    def reset(self) -> None:
        self._lib.sil_engine_reset(self._mem)

    def process(self, x: np.ndarray, block: int | None = None) -> np.ndarray:
        x = np.ascontiguousarray(x, dtype=np.int16)
        y = np.empty_like(x)
        if block is None:
            self._lib.sil_engine_process(self._mem, x, y, len(x))
            return y
        for i in range(0, len(x), block):
            xi = np.ascontiguousarray(x[i:i + block])
            yi = np.empty_like(xi)
            self._lib.sil_engine_process(self._mem, xi, yi, len(xi))
            y[i:i + block] = yi
        return y
