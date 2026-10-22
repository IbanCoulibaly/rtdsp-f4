"""Fault handling on the emulated board: crash -> record -> reset -> report.

A field device must never hang silently. These tests crash the firmware on
purpose and check that it reboots by itself, says why it rebooted, and
works normally afterwards.
"""

import numpy as np
import pytest

from rtdsp import analysis
from rtdsp import protocol as P

pytestmark = pytest.mark.emulator

CFSR_UNDEFINSTR = 1 << 16


def test_usage_fault_is_recorded_across_reset(board):
    board.inject_fault(P.DBG_FAULT_UDF)
    info = board.wait_boot(10.0)
    assert info.reset_reason == P.RESET_FAULT
    assert info.fault_cfsr & CFSR_UNDEFINSTR
    assert 0x08000000 <= info.fault_pc < 0x08100000      # PC of the faulting instruction, in flash
    assert info.fault_info == 6                          # exception 6 = UsageFault
    assert info.post_result == 0
    assert "*** FAULT" in board.qemu.console()
    x = analysis.test_signal(256)
    assert len(board.process(x)) == 256                  # fully operational again


def test_assert_is_recorded_with_line_number(board):
    board.inject_fault(P.DBG_FAULT_ASSERT)
    info = board.wait_boot(10.0)
    assert info.reset_reason == P.RESET_ASSERT
    assert info.fault_info > 0                           # __LINE__ of the configASSERT
    assert board.ping().reset_reason == P.RESET_ASSERT   # also reported on demand


def test_boot_counter_survives_soft_resets(board):
    assert len(board.boots) >= 3
    log = board.qemu.console()
    assert "boot#3" in log and np.all([s in log for s in ("FAULT", "ASSERT")])
