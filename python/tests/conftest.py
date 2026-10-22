"""Shared fixtures: designed filters, the SIL library, and a QEMU board."""

from __future__ import annotations

import numpy as np
import pytest

from rtdsp import catalog, emulator, sil


@pytest.fixture(scope="session")
def suite():
    return catalog.demo_filters()


@pytest.fixture(scope="session")
def sil_lib():
    try:
        return sil.load()
    except FileNotFoundError as e:
        pytest.skip(str(e))


@pytest.fixture(scope="session")
def rng():
    return np.random.default_rng(1234)


def _emulator_available() -> str | None:
    if emulator.find_qemu() is None:
        return "qemu-system-arm not installed"
    if not emulator.DEFAULT_ELF.exists():
        return f"{emulator.DEFAULT_ELF} not built (make fw)"
    return None


@pytest.fixture(scope="module")
def board():
    """One emulated board per test module (boots in well under a second)."""
    reason = _emulator_available()
    if reason:
        pytest.skip(reason)
    b = emulator.QemuBoard()
    dev = b.start()
    dev.boot_info = dev.wait_boot(10.0)
    dev.qemu = b
    yield dev
    b.stop()
