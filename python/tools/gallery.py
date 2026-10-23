#!/usr/bin/env python3
"""Generates docs/GALLERY.md and docs/img/gallery_*.png: one time-domain
input/output plot per catalog filter, each driven through the real board
(QEMU) rather than a Python-only simulation.

For every filter, python/rtdsp/catalog.py already specifies a `tones`
tuple -- showcase frequencies straddling the passband/stopband edge,
chosen for exactly this purpose. This script builds a multi-tone test
signal from those frequencies, uploads the filter and streams the signal
through the actual compiled firmware running under QEMU, and measures
the real per-tone gain at the output -- the same board, protocol, and
DSP kernels as every other result in this repository.

Run: python python/tools/gallery.py   (needs `make fw TARGET=qemu` first)
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rtdsp import FS_HZ, REPO_ROOT, analysis, catalog, fixedpoint
from rtdsp.emulator import QemuBoard

IMG_DIR = REPO_ROOT / "docs" / "img"
GALLERY_MD = REPO_ROOT / "docs" / "GALLERY.md"

# Same categorical palette as python/tools/demo.py, for visual consistency
# across every plot in this repository.
C_IN, C_OUT = "#2a78d6", "#1baf7a"
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "axes.edgecolor": INK_2,
    "axes.labelcolor": INK_2, "xtick.color": INK_2, "ytick.color": INK_2, "text.color": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 1.6, "font.size": 10, "legend.frameon": False,
})

# (catalog index, image slug, section, band description per tone)
GROUPS = [
    ("Low-pass", [0, 2]),
    ("High-pass", [3]),
    ("Band-pass", [1]),
    ("Notch", [4]),
]
SLUGS = ["kaiser_lowpass", "hamming_bandpass", "elliptic_lowpass", "chebyshev_highpass", "notch"]


def band_word(spec, f: float) -> str:
    """Classifies a showcase tone as pass/stop/reject based on the filter family."""
    p = spec.params
    if spec.family == "kaiser":
        return "passband" if f < p["fc"] else "stopband"
    if spec.family == "hamming":
        return "passband" if p["f1"] < f < p["f2"] else "stopband"
    if spec.family == "ellip":
        return "passband" if f < p["fc"] else "stopband"
    if spec.family == "cheby1":
        return "passband" if f > p["fc"] else "stopband"
    if spec.family == "notch":
        return "notch target" if abs(f - p["f0"]) < 5 else "passband"
    return "?"


def plot_filter(spec, x: np.ndarray, y: np.ndarray, gains: list[tuple[float, float, str]], path: Path) -> None:
    fig, (ax_t, ax_f) = plt.subplots(2, 1, figsize=(9.5, 6.4))

    settle = int(0.05 * FS_HZ)
    n = min(int(0.03 * FS_HZ), len(x) - settle)
    t_ms = np.arange(n) / FS_HZ * 1000
    ax_t.plot(t_ms, x[settle:settle + n], color=C_IN, label="input", alpha=0.85)
    ax_t.plot(t_ms, y[settle:settle + n], color=C_OUT, label="output (measured on QEMU STM32F405)")
    ax_t.set_xlabel("time (ms)")
    ax_t.set_ylabel("amplitude")
    ax_t.set_title(spec.desc.name, loc="left", fontsize=12, fontweight="bold")
    ax_t.legend(loc="upper right")

    for data, color, label in ((x, C_IN, "input"), (y, C_OUT, "output")):
        f, pxx = signal.welch(data.astype(float) / 32768, fs=FS_HZ, nperseg=4096)
        ax_f.semilogx(f[1:], 10 * np.log10(pxx[1:] + 1e-20), color=color, label=label)
    for f, gain_db, word in gains:
        ax_f.axvline(f, color=INK_2, linestyle=":", linewidth=1)
        ax_f.text(f, 4, f"{f:.0f} Hz\n{word}\n{gain_db:+.1f} dB", fontsize=7.5, color=INK_2,
                   ha="center", va="bottom")
    ax_f.set_xlabel("frequency (Hz)")
    ax_f.set_ylabel("PSD (dB/Hz)")
    ax_f.set_xlim(50, FS_HZ / 2)
    ax_f.set_ylim(-140, 20)
    ax_f.legend(loc="lower left")

    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)


def main() -> int:
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    specs = catalog.demo_specs()

    board = QemuBoard()
    dev = board.start()
    info = dev.wait_boot(10)
    print(f"booted: fw {info.fw_version} on {info.target}, POST {'PASS' if info.post_result == 0 else 'FAIL'}")

    results = []
    try:
        for i, spec in enumerate(specs):
            dev.set_filter(spec.desc)
            x = analysis.tones(16384, spec.tones, amp=0.28)
            y = dev.process(x)

            exact = np.array_equal(y, fixedpoint.engine(spec.desc, x))
            ref = (signal.lfilter(spec.ideal, 1.0, x.astype(float)) if spec.desc.type in (catalog.Q.FIR_Q15, catalog.Q.FIR_F32)
                   else signal.sosfilt(spec.ideal, x.astype(float)))
            sqnr = analysis.sqnr_db(ref, y)

            gains = [(f, analysis.tone_gain_db(x, y, f), band_word(spec, f)) for f in spec.tones]

            img_path = IMG_DIR / f"gallery_{SLUGS[i]}.png"
            plot_filter(spec, x, y, gains, img_path)

            results.append(dict(spec=spec, exact=exact, sqnr=sqnr, gains=gains,
                                 img=img_path.name, slug=SLUGS[i]))
            gain_str = ", ".join(f"{f:.0f}Hz({w}): {g:+.1f}dB" for f, g, w in gains)
            print(f"[{i}] {spec.desc.name}: bit-exact={exact} SQNR={sqnr:.1f}dB  {gain_str}")
    finally:
        board.stop()

    write_gallery_md(results)
    print(f"Wrote {GALLERY_MD.relative_to(REPO_ROOT)} and {len(results)} images to {IMG_DIR.relative_to(REPO_ROOT)}")
    return 0 if all(r["exact"] for r in results) else 1


def write_gallery_md(results: list[dict]) -> None:
    by_idx = {i: r for i, r in enumerate(results)}
    lines = [
        "# Filter gallery",
        "",
        "Every plot below was produced by `python/tools/gallery.py`: a multi-tone",
        "test signal is uploaded to the real firmware (the same ELF cross-compiled",
        "with `arm-none-eabi-gcc` for the STM32F405) running under QEMU, streamed",
        "through the active filter, and the result is measured back. Nothing here",
        "is computed in Python alone.",
        "",
        "Each test signal's frequencies come straight from",
        "[`python/rtdsp/catalog.py`](../python/rtdsp/catalog.py)'s `tones` field --",
        "chosen to straddle that filter's passband/stopband edge, so the effect is",
        "unambiguous: a tone either survives close to 0 dB or is measurably",
        "attenuated.",
        "",
        "Regenerate with:",
        "",
        "```bash",
        "make fw TARGET=qemu",
        "python python/tools/gallery.py",
        "```",
        "",
    ]

    for section, idxs in GROUPS:
        lines.append(f"## {section}")
        lines.append("")
        for i in idxs:
            r = by_idx[i]
            spec, exact, sqnr, gains = r["spec"], r["exact"], r["sqnr"], r["gains"]
            lines.append(f"### {spec.desc.name}")
            lines.append("")
            lines.append(f"![{spec.desc.name}](img/{r['img']})")
            lines.append("")

            gain_desc = "; ".join(
                f"**{f:.0f} Hz** ({word}) measured at **{g:+.1f} dB**" for f, g, word in gains
            )
            lines.append(
                f"{describe(spec)} Driving it with {', '.join(f'{f:.0f} Hz' for f, *_ in gains)} tones "
                f"simultaneously: {gain_desc}. Output matched the bit-exact reference model: "
                f"**{'yes' if exact else 'NO'}**. Measured SQNR against the float64 design: **{sqnr:.1f} dB**."
            )
            lines.append("")

    GALLERY_MD.write_text("\n".join(lines) + "\n")


def describe(spec) -> str:
    p = spec.params
    if spec.family == "kaiser":
        return (f"A {p['fc']} Hz low-pass FIR filter (Kaiser window, {p['atten']} dB stop-band "
                f"attenuation target), running in Q15 fixed point -- the format a 16-bit ADC/DAC "
                f"would deliver directly.")
    if spec.family == "ellip":
        return (f"A {p['fc']} Hz low-pass elliptic filter, order {p['order']} ({p['rp']} dB pass-band "
                f"ripple, {p['rs']} dB stop-band attenuation), run as 3 cascaded Q31 biquad sections. "
                f"Elliptic filters trade pass-band ripple for the steepest possible roll-off at a given order.")
    if spec.family == "cheby1":
        return (f"A {p['fc']} Hz high-pass Chebyshev Type I filter, order {p['order']} "
                f"({p['rp']} dB pass-band ripple), run as cascaded float32 biquads.")
    if spec.family == "hamming":
        return (f"A {p['f1']}-{p['f2']} Hz band-pass FIR filter (Hamming window), run in float32 "
                f"directly on the samples with no fixed-point quantisation step.")
    if spec.family == "notch":
        return (f"A notch filter targeting exactly {p['f0']} Hz with Q = {p['q']} (the classic "
                f"50/60 Hz mains-hum remover, retuned here to a more audible frequency for the "
                f"demo), run as a single Q31 biquad section.")
    return ""


if __name__ == "__main__":
    raise SystemExit(main())
