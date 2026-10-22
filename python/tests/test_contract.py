"""Contract tests: the Python mirror of the protocol must equal the C headers."""

import re

from rtdsp import REPO_ROOT
from rtdsp import protocol as P
from rtdsp import quantize as Q

PROTO_H = REPO_ROOT / "libs/link/include/link/protocol_defs.h"
ENGINE_H = REPO_ROOT / "libs/dsp/include/dsp/filter_engine.h"


def c_defines(path):
    out = {}
    for name, value in re.findall(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", path.read_text(), re.M):
        out[name] = int(value, 0)
    return out


def test_every_c_protocol_constant_is_mirrored():
    defs = c_defines(PROTO_H)
    prefixes = ("MSG_", "ST_", "MODE_", "DBG_FAULT_", "RESET_", "POST_FAIL_")
    checked = 0
    for name, value in defs.items():
        if name.startswith(prefixes):
            assert getattr(P, name) == value, name
            checked += 1
    assert checked > 30
    assert P.PROTOCOL_VERSION == defs["LINK_PROTOCOL_VERSION"]
    assert P.MAX_PAYLOAD == defs["LINK_MAX_PAYLOAD"]


def test_payload_layouts_match():
    defs = c_defines(PROTO_H)
    assert P.DEVINFO_LEN == defs["LINK_DEVINFO_LEN"]
    assert P.STATS_LEN == defs["LINK_STATS_LEN"]
    text = PROTO_H.read_text()
    assert f'"{P.DEVINFO_FMT}"' in text and f'"{P.STATS_FMT}"' in text


def test_filter_types_and_limits_match():
    text = ENGINE_H.read_text()
    enum = dict((k, int(v)) for k, v in re.findall(r"FILTER_(\w+)\s*=\s*(\d+)", text))
    assert enum["BYPASS"] == Q.BYPASS
    assert enum["FIR_Q15"] == Q.FIR_Q15
    assert enum["FIR_F32"] == Q.FIR_F32
    assert enum["BIQUAD_Q31"] == Q.BIQUAD_Q31
    assert enum["BIQUAD_F32"] == Q.BIQUAD_F32
    defs = {k: int(v) for k, v in re.findall(r"#define\s+(FILTER_MAX_\w+)\s+(\d+)u", text)}
    assert defs["FILTER_MAX_TAPS"] == Q.MAX_TAPS
    assert defs["FILTER_MAX_STAGES"] == Q.MAX_STAGES
    assert defs["FILTER_MAX_POST_SHIFT"] == Q.MAX_POST_SHIFT
