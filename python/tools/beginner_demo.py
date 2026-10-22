#!/usr/bin/env python3
"""Generates docs/img/filters_explained.png: a plain-language, no-jargon
"before vs. after" picture of what each filter shape actually does to a
sound wave.

Every waveform below is produced by SilEngine, i.e. the *real* compiled
C filter engine (libs/dsp/src/biquad.c) that also runs on the board --
not a separate, simplified re-implementation. See python/rtdsp/sil.py.

Run: python python/tools/beginner_demo.py   (needs `make sil` first)
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rtdsp import FS_HZ, REPO_ROOT, design, quantize as Q, sil

OUT = REPO_ROOT / "docs" / "img" / "filters_explained.png"


def tone(freq_hz: float, amp: float, duration_s: float, phase: float = 0.0) -> np.ndarray:
    t = np.arange(int(duration_s * FS_HZ)) / FS_HZ
    return amp * np.sin(2 * np.pi * freq_hz * t + phase)


def to_pcm(x: np.ndarray) -> np.ndarray:
    peak = np.max(np.abs(x))
    if peak > 32000:
        x = x * (32000.0 / peak)
    return x.astype(np.int16)


def run(sos: np.ndarray, x_pcm: np.ndarray) -> np.ndarray:
    desc = Q.biquad_f32(sos)
    engine = sil.SilEngine(sil.load(), desc)
    return engine.process(x_pcm, block=32)


SCENARIOS = [
    dict(
        key="lowpass",
        title="Low-pass: smooths out fast, jittery noise",
        caption="Keeps the slow, steady wave. Removes the fast hiss on top of it.",
        tech="Butterworth low-pass, cutoff 800 Hz, 4th order",
        build=lambda: (
            tone(150, 6000, 0.3) + 2200 * np.random.default_rng(1).standard_normal(int(0.3 * FS_HZ)),
            design.butter_sos(4, 800, btype="lowpass"),
        ),
        window_ms=60,
    ),
    dict(
        key="highpass",
        title="High-pass: removes slow drift, keeps the quick wiggle",
        caption="The wandering baseline disappears. The fast wiggle rides flat and centered.",
        tech="Butterworth high-pass, cutoff 200 Hz, 4th order",
        build=lambda: (
            tone(600, 4000, 0.3) + tone(3, 9000, 0.3),
            design.butter_sos(4, 200, btype="highpass"),
        ),
        window_ms=60,
    ),
    dict(
        key="bandpass",
        title="Band-pass: keeps the middle pitch only",
        caption="The low rumble and the high whistle both vanish. Only the middle tone survives.",
        tech="Butterworth band-pass, 500-2000 Hz, 4th order",
        build=lambda: (
            tone(80, 4500, 0.3) + tone(1000, 4500, 0.3) + tone(6000, 3200, 0.3),
            design.butter_sos(4, [500, 2000], btype="bandpass"),
        ),
        window_ms=25,
    ),
    dict(
        key="notch",
        title="Notch: removes one specific, annoying pitch",
        caption="Classic use: killing 60 Hz electrical hum while leaving everything else untouched.",
        tech="RBJ notch, 60 Hz, Q = 8",
        build=lambda: (
            tone(400, 5000, 0.3) + tone(60, 5000, 0.3),
            design.notch_sos(60, 8),
        ),
        window_ms=60,
    ),
]

COLORS = {"lowpass": "#2563eb", "highpass": "#059669", "bandpass": "#d97706", "notch": "#dc2626"}


def main() -> int:
    fig, axes = plt.subplots(2, 2, figsize=(13, 9.5))
    fig.suptitle("What each filter shape actually does to a sound wave", fontsize=18, fontweight="bold", y=0.995)
    fig.text(0.5, 0.955,
              "Every curve here comes from the project's real, compiled filter code -- not a simplified stand-in.",
              ha="center", fontsize=10.5, color="#4b5563")

    for ax, spec in zip(axes.flat, SCENARIOS):
        raw, sos = spec["build"]()
        x = to_pcm(raw)
        y = run(sos, x)

        settle = int(0.08 * FS_HZ)  # let the filter settle before we display anything
        n = int(spec["window_ms"] / 1000 * FS_HZ)
        t_ms = np.arange(n) / FS_HZ * 1000

        ax.plot(t_ms, x[settle:settle + n], color="#9ca3af", linewidth=1.1, label="Before")
        ax.plot(t_ms, y[settle:settle + n], color=COLORS[spec["key"]], linewidth=2.0, label="After")

        ax.set_title(spec["title"], fontsize=13, fontweight="bold", pad=10)
        ax.set_xlabel("time  →", fontsize=9.5, color="#6b7280")
        ax.set_yticks([])
        ax.set_xticks([])
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color("#d1d5db")
        ax.legend(loc="upper right", frameon=False, fontsize=9.5)

        ax.text(0.0, -0.16, spec["caption"], transform=ax.transAxes, fontsize=10, color="#374151")
        ax.text(0.0, -0.26, spec["tech"], transform=ax.transAxes, fontsize=8.5, color="#9ca3af")

    fig.tight_layout(rect=(0, 0.01, 1, 0.92), h_pad=5.5)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=160)
    plt.close(fig)
    print(f"Wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
