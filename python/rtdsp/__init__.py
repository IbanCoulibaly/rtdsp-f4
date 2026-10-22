"""RTDSP-F4 host tooling: filter design, quantisation, bit-exact models,
the wire protocol, and drivers for the SIL library and the QEMU board."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FS_HZ = 48_000
BLOCK_MAX = 64
