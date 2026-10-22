#!/usr/bin/env python3
"""End-to-end demo: design -> quantise -> upload to the emulated STM32 ->
stream test signals -> measure -> report.

Outputs build/reports/report.md plus one PNG per filter and one for the
on-board signal generator. Run with `make demo`.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rtdsp import FS_HZ, REPO_ROOT, analysis, catalog, fixedpoint  # noqa: E402
from rtdsp import quantize as Q  # noqa: E402
from rtdsp.emulator import QemuBoard  # noqa: E402

OUT = REPO_ROOT / "build" / "reports"

# Reference categorical palette (slots 1-3, fixed order) and ink tokens.
C_IDEAL, C_QUANT, C_BOARD = "#2a78d6", "#eb6834", "#1baf7a"
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "axes.edgecolor": INK_2,
    "axes.labelcolor": INK_2, "xtick.color": INK_2, "ytick.color": INK_2, "text.color": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 2.0, "font.size": 10, "legend.frameon": False,
})


def ideal_response(desc: Q.FilterDesc, sos_or_h, f):
    if desc.type in (Q.FIR_Q15, Q.FIR_F32):
        return signal.freqz(sos_or_h, worN=f, fs=FS_HZ)[1]
    return signal.sosfreqz(sos_or_h, worN=f, fs=FS_HZ)[1]


def measured_response(x: np.ndarray, y: np.ndarray, nperseg: int = 2048):
    """H(f) = S_xy(f) / S_xx(f): the transfer function estimated from the
    board's actual input/output (Welch-averaged cross-spectrum)."""
    f, sxy = signal.csd(x.astype(float), y.astype(float), fs=FS_HZ, nperseg=nperseg)
    _, sxx = signal.welch(x.astype(float), fs=FS_HZ, nperseg=nperseg)
    return f, sxy / sxx


def plot_filter(idx: int, desc: Q.FilterDesc, ideal, x, y, stats_line: str) -> Path:
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 3.8), gridspec_kw={"width_ratios": [2.2, 1]})
    f = np.linspace(10, FS_HZ / 2, 2048)
    fm, hm = measured_response(x, y)
    ax1.semilogx(f, analysis.mag_db(ideal_response(desc, ideal, f)), color=C_IDEAL, label="design (float64)")
    fq, hq = analysis.response(desc, n=2048)
    ax1.semilogx(fq[1:], analysis.mag_db(hq[1:]), color=C_QUANT, linestyle=(0, (4, 3)), label="quantised coefficients")
    keep = fm > 20
    ax1.semilogx(fm[keep], analysis.mag_db(hm[keep]), color=C_BOARD, linewidth=0, marker="o", markersize=3,
                 markevery=4, label="measured on emulated STM32")
    ax1.set_ylim(-100, 10)
    ax1.set_xlim(20, FS_HZ / 2)
    ax1.set_xlabel("frequency (Hz)")
    ax1.set_ylabel("|H| (dB)")
    ax1.set_title(desc.name, loc="left", fontsize=11)
    ax1.legend(loc="lower left")

    if desc.type in (Q.BIQUAD_Q31, Q.BIQUAD_F32):
        z, p, _ = signal.sos2zpk(Q.quantised_sos(desc))
        t = np.linspace(0, 2 * np.pi, 400)
        ax2.plot(np.cos(t), np.sin(t), color=INK_2, linewidth=1)
        ax2.plot(z.real, z.imag, "o", markerfacecolor="none", markeredgecolor=C_IDEAL, markersize=8, label="zeros")
        ax2.plot(p.real, p.imag, "x", color=C_QUANT, markersize=8, markeredgewidth=2, label="poles")
        ax2.set_aspect("equal")
        ax2.set_title(f"z-plane, max |p| = {np.max(np.abs(p)):.4f}", loc="left", fontsize=10)
        ax2.legend(loc="upper right", fontsize=8)
    else:
        taps = Q.quantised_fir(desc)
        ax2.vlines(np.arange(len(taps)), 0, taps, color=C_IDEAL, linewidth=1.5)
        ax2.axhline(0, color=INK_2, linewidth=0.8)
        ax2.set_title(f"impulse response h[n], {len(taps)} taps", loc="left", fontsize=10)
        ax2.set_xlabel("n")
    fig.text(0.01, 0.01, stats_line, color=INK_2, fontsize=8.5)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    path = OUT / f"filter_{idx}.png"
    fig.savefig(path, dpi=130)
    plt.close(fig)
    return path


def plot_siggen(xin, xout, desc) -> Path:
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6))
    n = np.arange(600)
    ax1.plot(n / FS_HZ * 1e3, xin[-600:] / 32768, color=C_IDEAL, label="ISR input (1 kHz + 3 kHz + noise)")
    ax1.plot(n / FS_HZ * 1e3, xout[-600:] / 32768, color=C_QUANT, label="DSP task output")
    ax1.set_xlabel("time (ms)")
    ax1.set_ylabel("amplitude (FS)")
    ax1.set_title(f"On-board signal generator through {desc.name}", loc="left", fontsize=11)
    ax1.set_ylim(-0.8, 1.05)
    ax1.legend(loc="upper right", ncol=2)
    for data, color, label in ((xin, C_IDEAL, "input"), (xout, C_QUANT, "output")):
        f, pxx = signal.welch(data / 32768, fs=FS_HZ, nperseg=4096)
        ax2.semilogx(f[1:], 10 * np.log10(pxx[1:] + 1e-20), color=color, label=label)
    ax2.set_xlabel("frequency (Hz)")
    ax2.set_ylabel("PSD (dB/Hz)")
    ax2.set_xlim(50, FS_HZ / 2)
    ax2.legend(loc="upper right")
    fig.tight_layout()
    path = OUT / "siggen.png"
    fig.savefig(path, dpi=130)
    plt.close(fig)
    return path


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    specs = catalog.demo_specs()
    filters = [sp.desc for sp in specs]
    ideals = [sp.ideal for sp in specs]
    rows = []
    board = QemuBoard()
    dev = board.start()
    info = dev.wait_boot(10)
    print(f"booted: fw {info.fw_version} on {info.target}, POST {'PASS' if info.post_result == 0 else 'FAIL'}")
    try:
        x = analysis.test_signal(16384, seed=42)
        for i, (desc, ideal) in enumerate(zip(filters, ideals)):
            dev.set_filter(desc)
            t0 = time.monotonic()
            y = dev.process(x)
            dt = time.monotonic() - t0
            st = dev.stats()
            exact = np.array_equal(y, fixedpoint.engine(desc, x))
            ref = (signal.lfilter(ideal, 1.0, x.astype(float)) if desc.type in (Q.FIR_Q15, Q.FIR_F32)
                   else signal.sosfilt(ideal, x.astype(float)))
            sqnr = analysis.sqnr_db(ref, y)
            line = (f"bit-exact vs model: {'yes' if exact else 'NO'}   SQNR vs float64 design: {sqnr:.1f} dB   "
                    f"{st.cycles_per_sample:.0f} cycles/sample ({100 * st.dsp_utilisation:.1f} % of budget)")
            png = plot_filter(i, desc, ideal, x, y, line)
            rows.append((desc, exact, sqnr, st, png, len(x) / dt))
            print(f"[{i}] {desc.name}: {line}")

        notch = filters[4]
        dev.set_filter(notch)
        dev.stream.clear()
        dev.set_mode_siggen(1000, 0.4, 3000, 0.4, 0.02)
        xin, xout = dev.capture_stream(48000)
        dev.set_mode_host()
        sg_exact = np.array_equal(xout, fixedpoint.engine(notch, xin))
        sg_png = plot_siggen(xin, xout, notch)
        st = dev.stats()
    finally:
        console = board.console()
        board.stop()

    with open(OUT / "report.md", "w") as fh:
        fh.write("# RTDSP-F4 demo report\n\n")
        fh.write(f"Firmware {info.fw_version} on `{info.target}` (built {info.build}), POST "
                 f"{'PASS' if info.post_result == 0 else f'FAIL {info.post_result:#x}'}.\n\n")
        fh.write("| filter | kernel | order | post_shift | bit-exact | SQNR vs float64 | cycles/sample | budget used |\n")
        fh.write("|---|---|---|---|---|---|---|---|\n")
        for desc, exact, sqnr, s, png, _ in rows:
            fh.write(f"| {desc.name} | {Q.TYPE_NAMES[desc.type]} | {desc.order} | {desc.post_shift} | "
                     f"{'yes' if exact else '**NO**'} | {sqnr:.1f} dB | {s.cycles_per_sample:.0f} | "
                     f"{100 * s.dsp_utilisation:.1f} % |\n")
        fh.write("\nCycle counts under QEMU come from an emulated timer and are indicative only; "
                 "QEMU is not cycle-accurate.\n\n")
        for desc, *_rest in rows:
            fh.write(f"![{desc.name}]({_rest[3].name})\n\n")
        fh.write(f"## Signal generator (ISR-driven)\n\n{len(xin)} samples, output bit-exact: "
                 f"{'yes' if sg_exact else 'NO'}, 1 kHz rejection "
                 f"{analysis.tone_gain_db(xin, xout, 1000):.1f} dB, overruns {st.siggen_overruns}, "
                 f"TX drops {st.tx_dropped}.\n\n![siggen]({sg_png.name})\n\n")
        fh.write("## Board console\n\n```\n" + console + "\n```\n")
    print(f"report: {(OUT / 'report.md').relative_to(REPO_ROOT)}")
    return 0 if all(r[1] for r in rows) and sg_exact else 1


if __name__ == "__main__":
    raise SystemExit(main())
