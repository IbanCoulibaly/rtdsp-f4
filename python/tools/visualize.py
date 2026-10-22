#!/usr/bin/env python3
"""Interactive filter viewer.

Window 1 ("signals"): the input x[n] on top and the filtered output y[n]
from the emulated STM32 at the bottom, each as a waveform and a spectrum.
Window 2 ("equations"): the mathematics of the selected filter with its
actual coefficients, as they run on the board.

    python python/tools/visualize.py               # all filters, Prev/Next or arrow keys
    python python/tools/visualize.py --filter 2    # start at filter 2
    python python/tools/visualize.py --source model    # no emulator: Python bit-exact model
    python python/tools/visualize.py --save build/viz  # write PNGs instead of opening windows
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ARGS = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ARGS.add_argument("--filter", type=int, default=0, help="index of the filter to show first (0-4)")
ARGS.add_argument("--source", choices=("board", "model"), default="board",
                  help="board: run on the QEMU STM32 (default); model: Python bit-exact model only")
ARGS.add_argument("--save", type=Path, help="render every filter to PNGs in this directory, no windows")

if __name__ == "__main__" and "--save" in sys.argv:
    import matplotlib

    matplotlib.use("Agg")  # headless rendering

import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.widgets import Button  # noqa: E402
from scipy import signal  # noqa: E402

from rtdsp import FS_HZ, analysis, catalog, fixedpoint  # noqa: E402
from rtdsp import quantize as Q  # noqa: E402
from rtdsp.catalog import FilterSpec  # noqa: E402

# Reference categorical palette, fixed order: input = slot 1, output = slot 2.
C_IN, C_OUT = "#2a78d6", "#eb6834"
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
N_SAMPLES = 9600            # 200 ms at 48 kHz
VIEW = slice(4800, 5280)    # 10 ms of steady state for the waveform panels
SETTLE = 2048               # skip the start-up transient in the spectra

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "axes.edgecolor": INK_2,
    "axes.labelcolor": INK_2, "xtick.color": INK_2, "ytick.color": INK_2, "text.color": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 1.6, "font.size": 10, "legend.frameon": False,
    "mathtext.fontset": "cm",
})


@dataclass
class Run:
    spec: FilterSpec
    x: np.ndarray
    y: np.ndarray
    source: str
    bit_exact: bool


# ----------------------------------------------------------------------------- data

def showcase_input(spec: FilterSpec, seed: int) -> np.ndarray:
    """Tones on both sides of the band edge plus a little white noise, so the
    filter's job is visible in the waveform and in the spectrum."""
    amp = 0.85 / len(spec.tones)
    x = analysis.tones(N_SAMPLES, spec.tones, amp).astype(float) / 32768
    x += np.random.default_rng(seed).uniform(-0.01, 0.01, N_SAMPLES)
    return np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16)


def collect(specs: list[FilterSpec], source: str) -> list[Run]:
    inputs = [showcase_input(s, i) for i, s in enumerate(specs)]
    models = [fixedpoint.engine(s.desc, x) for s, x in zip(specs, inputs)]
    if source == "board":
        from rtdsp.emulator import QemuBoard, find_qemu  # noqa: PLC0415

        if find_qemu() is None:
            print("qemu-system-arm not found: falling back to the Python bit-exact model")
        else:
            board = QemuBoard()
            try:
                dev = board.start()
                info = dev.wait_boot(10)
                print(f"emulated STM32 booted (fw {info.fw_version}, POST "
                      f"{'PASS' if info.post_result == 0 else 'FAIL'})")
                runs = []
                for s, x, m in zip(specs, inputs, models):
                    dev.set_filter(s.desc)
                    y = dev.process(x)
                    runs.append(Run(s, x, y, "emulated STM32F405", bool(np.array_equal(y, m))))
                    print(f"  {s.desc.name}: {len(y)} samples, bit-exact = {runs[-1].bit_exact}")
                return runs
            finally:
                board.stop()
    return [Run(s, x, m, "Python bit-exact model", True) for s, x, m in zip(specs, inputs, models)]


def spectrum_dbfs(v: np.ndarray):
    seg = v[SETTLE:].astype(float) / 32768
    w = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * w)) * 2 / np.sum(w)
    return np.fft.rfftfreq(len(seg), 1 / FS_HZ), 20 * np.log10(np.maximum(mag, 1e-7))


def khz(f: float) -> str:
    return f"{f / 1000:g} kHz" if f >= 1000 else f"{f:g} Hz"


# -------------------------------------------------------------------- signal window

def draw_signals(fig, run: Run, idx: int, total: int) -> None:
    fig.clear()
    gs = fig.add_gridspec(2, 2, width_ratios=[1.55, 1], left=0.07, right=0.98, top=0.85, bottom=0.15,
                          hspace=0.42, wspace=0.18)
    t_ms = np.arange(N_SAMPLES)[VIEW] / FS_HZ * 1e3
    ax_xt = fig.add_subplot(gs[0, 0])
    ax_yt = fig.add_subplot(gs[1, 0], sharex=ax_xt, sharey=ax_xt)
    ax_xf = fig.add_subplot(gs[0, 1])
    ax_yf = fig.add_subplot(gs[1, 1], sharex=ax_xf, sharey=ax_xf)

    ax_xt.plot(t_ms, run.x[VIEW] / 32768, color=C_IN)
    ax_yt.plot(t_ms, run.y[VIEW] / 32768, color=C_OUT)
    ax_xt.set_ylim(-1.05, 1.05)
    ax_xt.set_ylabel("x[n]  (full scale)")
    ax_yt.set_ylabel("y[n]  (full scale)")
    ax_yt.set_xlabel("time (ms)")
    ax_xt.set_title(f"INPUT  -  tones at {', '.join(khz(f) for f in run.spec.tones)} + noise",
                    loc="left", fontsize=11, color=INK)
    check = "bit-exact vs model" if run.bit_exact else "MISMATCH vs model"
    ax_yt.set_title(f"OUTPUT  -  {run.source}  ({check})", loc="left", fontsize=11, color=INK)

    for ax, v, color in ((ax_xf, run.x, C_IN), (ax_yf, run.y, C_OUT)):
        f, db = spectrum_dbfs(v)
        ax.semilogx(f[1:], db[1:], color=color, linewidth=1.2)
        ax.set_xlim(50, FS_HZ / 2)
        ax.set_ylim(-110, 5)
        ax.set_ylabel("dBFS")
    ax_xf.set_title("input spectrum", loc="left", fontsize=10, color=INK_2)
    ax_yf.set_title("output spectrum", loc="left", fontsize=10, color=INK_2)
    ax_yf.set_xlabel("frequency (Hz)")

    # Label each tone on the output spectrum with the gain the board applied.
    for tone in run.spec.tones:
        g = analysis.tone_gain_db(run.x, run.y, tone)
        f, db = spectrum_dbfs(run.y)
        level = db[np.argmin(np.abs(f - tone))]
        passed = g > -3
        # Passed tones are labelled just above their peak; rejected ones above the
        # noise floor, with a leader line down to where the tone went.
        ax_yf.annotate(f"{khz(tone)}\n{g:+.1f} dB ({'passed' if passed else 'rejected'})", (tone, level),
                       xytext=(0, 14) if passed else (tone, -42), textcoords="offset points" if passed else "data",
                       ha="center", fontsize=8.5, color=INK,
                       bbox={"boxstyle": "round,pad=0.2", "fc": SURFACE, "ec": "none"},
                       arrowprops={"arrowstyle": "-", "color": INK_2, "lw": 0.8})

    fig.suptitle(f"[{idx + 1}/{total}]  {run.spec.desc.name}", x=0.07, y=0.975, ha="left", fontsize=13,
                 fontweight="bold")
    fig.text(0.07, 0.925, "top: what goes into the board    bottom: what the DSP task sends back    "
             "(arrow keys / buttons: other filters)", fontsize=9, color=INK_2)


# ------------------------------------------------------------------ equation window

def _section_rows(desc: Q.FilterDesc) -> list[str]:
    sos = Q.quantised_sos(desc)
    rows = ["sec      b0          b1          b2          a1          a2        |pole|   |a2|<1  |a1|<1+a2"]
    for k, (b0, b1, b2, _, a1, a2) in enumerate(sos, 1):
        r = np.max(np.abs(np.roots([1.0, a1, a2])))
        ok1 = "yes" if abs(a2) < 1 else "NO"
        ok2 = "yes" if abs(a1) < 1 + a2 else "NO"
        rows.append(f" {k}   {b0:+.6f}  {b1:+.6f}  {b2:+.6f}  {a1:+.6f}  {a2:+.6f}   {r:.5f}    {ok1:<3}     {ok2}")
    return rows


def _f32(v) -> str:
    """Shortest decimal that rounds back to exactly this float32 value."""
    f = np.float32(v)
    if f != 0 and abs(f) < 1e-3:
        return np.format_float_scientific(f, unique=True, trim="-", exp_digits=1)
    return np.format_float_positional(f, unique=True, trim="-")


def _poly_tex(coeffs, fmt) -> str:
    """c0 + c1 z^-1 + c2 z^-2 with proper signs, as mathtext."""
    out = []
    for i, c in enumerate(coeffs):
        mag = fmt(abs(c))
        term = mag if i == 0 else rf"{mag}\,z^{{-{i}}}"
        sign = ("-" if c < 0 else "") if i == 0 else (" - " if c < 0 else " + ")
        out.append(sign + term)
    return "".join(out)


def _rows(values, per_row: int, width: int) -> list[str]:
    cells = [f" {v:>{width}}" for v in values]
    return ["".join(cells[i:i + per_row]) for i in range(0, len(cells), per_row)]


def exact_tf_lines(spec: FilterSpec) -> list[tuple[str, str]]:
    """H(z) written with the coefficients exactly as the board stores them.

    Q15 FIR:   h[k] = c_k / 2^15, c_k the int16 taps          -> exact integers
    Q31 IIR:   multiply each section by 2^(31-s)/2^(31-s)      -> exact integers
    float32:   shortest decimals that round-trip to the stored float32
    """
    d = spec.desc
    L: list[tuple[str, str]] = [("h", "Exact transfer function  (coefficients exactly as executed on the board)")]
    if d.type in (Q.FIR_Q15, Q.FIR_F32):
        n = d.order
        c = np.asarray(d.coeffs)
        m = (n - 1) // 2
        symmetric = n % 2 == 1 and np.array_equal(c, c[::-1])
        scale = r"2^{-15}" if d.type == Q.FIR_Q15 else ""
        if symmetric:
            body = (rf"\sum_{{k=0}}^{{{m - 1}}} c_k\left(z^{{-k}} + z^{{-({n - 1}-k)}}\right) + c_{{{m}}}\,z^{{-{m}}}")
            note = f"symmetric taps (c_k = c_{n - 1}-k), so only c_0 ... c_{m} are listed"
            listed = c[: m + 1]
        else:
            body = rf"\sum_{{k=0}}^{{{n - 1}}} c_k\,z^{{-k}}"
            note = "all taps listed"
            listed = c
        L += [("m", rf"$H(z) = {scale}\left[{body}\right]$" if scale else rf"$H(z) = {body}$")]
        if d.type == Q.FIR_Q15:
            L += [("t", f"c_k = int16 Q15 taps; {note}:")]
            L += [("c", r) for r in _rows([int(v) for v in listed], 15, 6)]
        else:
            L += [("t", f"c_k = float32 taps (shortest exact decimal); {note}:")]
            L += [("c", r) for r in _rows([_f32(v) for v in listed], 8, 13)]
        return L

    if d.type == Q.BIQUAD_Q31:
        one = 2 ** (31 - d.post_shift)
        for k, (b0, b1, b2, na1, na2) in enumerate(np.asarray(d.coeffs, dtype=np.int64), 1):
            num = _poly_tex([b0, b1, b2], lambda v: f"{int(v)}")
            den = _poly_tex([one, -na1, -na2], lambda v: f"{int(v)}")
            L += [("m", rf"$H_{k}(z) = \dfrac{{{num}}}{{{den}}}$")]
        L += [("t", f"integers = Q{31 - d.post_shift} coefficients; numerator and denominator both carry the "
                    f"factor 2^{31 - d.post_shift} = {one}, so these ratios are exact")]
    else:
        for k, (b0, b1, b2, na1, na2) in enumerate(np.asarray(d.coeffs, dtype=np.float32), 1):
            num = _poly_tex([b0, b1, b2], _f32)
            den = _poly_tex([np.float32(1.0), -na1, -na2], _f32)
            L += [("m", rf"$H_{k}(z) = \dfrac{{{num}}}{{{den}}}$")]
        L += [("t", "float32 coefficients, shortest decimals that round-trip exactly to the stored values")]
    if d.order > 1:
        L += [("m", r"$H(z) = " + r"\,".join(f"H_{k}(z)" for k in range(1, d.order + 1)) + "$")]
    sos = Q.quantised_sos(d)
    b, a = np.array([1.0]), np.array([1.0])
    for row in sos:
        b, a = np.polymul(b, row[:3]), np.polymul(a, row[3:])
    L += [("t", f"expanded to one rational function of order {2 * d.order} (equivalent, float64 rounding):")]
    L += [("c", "b = [" + ", ".join(f"{v:.10g}" for v in b) + "]"),
          ("c", "a = [" + ", ".join(f"{v:.10g}" for v in a) + "]")]
    return L


def equation_lines(spec: FilterSpec) -> list[tuple[str, str]]:
    """(kind, text) lines: 'h' heading, 'm' mathtext, 't' prose, 'c' monospace."""
    d, p = spec.desc, spec.params
    fs = FS_HZ
    L: list[tuple[str, str]] = exact_tf_lines(spec)

    if d.type in (Q.FIR_Q15, Q.FIR_F32):
        h = Q.quantised_fir(d)
        n = len(h)
        L += [("h", "Difference equation  (DSP task, every sample)"),
              ("m", r"$y[n] \;=\; \sum_{k=0}^{N-1} h[k]\,x[n-k]$" + rf"$\qquad N = {n}$ taps"),
              ("h", "Transfer function"),
              ("m", r"$H(z) \;=\; \sum_{k=0}^{N-1} h[k]\,z^{-k}$" +
               r"$\qquad$ only zeros $\Rightarrow$ always stable"),
              ("h", "Design")]
        if spec.family == "kaiser":
            beta = 0.1102 * (p["atten"] - 8.7)
            dw = 2 * np.pi * p["transition"] / fs
            L += [("m", r"$h[k] = w_K[k]\;\dfrac{2f_c}{f_s}\,\mathrm{sinc}\!\left(\dfrac{2f_c}{f_s}(k-M)\right),"
                        r"\quad M=\dfrac{N-1}{2}$" + rf"$\qquad f_c = {p['fc']}$ Hz"),
                  ("m", rf"$\beta = 0.1102\,(A-8.7) = {beta:.3f}\qquad "
                        rf"N \approx \dfrac{{A-7.95}}{{2.285\,\Delta\omega}}+1 = "
                        rf"{(p['atten'] - 7.95) / (2.285 * dw) + 1:.1f} \rightarrow {n}$"
                        rf"$\qquad A = {p['atten']}$ dB, $\Delta f = {p['transition']}$ Hz")]
        else:
            L += [("m", r"$h_d[k] = \dfrac{2f_2}{f_s}\mathrm{sinc}\!\left(\dfrac{2f_2}{f_s}(k-M)\right)"
                        r" - \dfrac{2f_1}{f_s}\mathrm{sinc}\!\left(\dfrac{2f_1}{f_s}(k-M)\right)$"
                        + rf"$\qquad f_1 = {p['f1']},\; f_2 = {p['f2']}$ Hz"),
                  ("m", r"$h[k] = h_d[k]\,w[k],\qquad w[k] = 0.54 - 0.46\cos\!\left(\dfrac{2\pi k}{N-1}\right)$"
                        r"$\quad$ (Hamming)")]
        L += [("h", "Linear phase"),
              ("m", rf"$h[k] = h[N-1-k]\;\Rightarrow\;\tau_g = \dfrac{{N-1}}{{2}} = {(n - 1) // 2}$ samples"
                    rf" $= {(n - 1) / 2 / fs * 1e3:.3f}$ ms for every frequency")]
        if d.type == Q.FIR_Q15:
            L += [("h", "On the board: Q15 fixed point, 64-bit accumulator"),
                  ("m", r"$h_{Q15}[k] = \mathrm{round}\!\left(h[k]\cdot 2^{15}\right)\qquad "
                        r"y[n] = \mathrm{sat}_{16}\!\left(\left(\sum_k h_{Q15}[k]\,x[n-k] + 2^{14}\right)"
                        r" \gg 15\right)$")]
        else:
            L += [("h", "On the board: IEEE-754 float32 on the Cortex-M4 FPU"),
                  ("m", r"$x_f = x_{Q15}\cdot 2^{-15},\qquad y_f = \sum_k h[k]\,x_f[n-k],\qquad "
                        r"y_{Q15} = \mathrm{sat}_{16}\!\left(\mathrm{round}(y_f\cdot 2^{15})\right)$")]
        mid = (n - 1) // 2
        L += [("h", "Coefficients as stored on the board"),
              ("c", "   k  " + "".join(f"{k:>11d}" for k in (0, 1, 2, 3, mid - 1, mid, mid + 1))),
              ("c", "h[k]  " + "".join(f"{h[k]:>+11.6f}" for k in (0, 1, 2, 3, mid - 1, mid, mid + 1)))]
        if d.type == Q.FIR_Q15:
            L += [("c", " int  " + "".join(f"{int(d.coeffs[k]):>11d}" for k in (0, 1, 2, 3, mid - 1, mid, mid + 1)))]
        L += [("t", f"sum |h[k]| = {Q.fir_l1_gain(h):.3f}  (worst-case gain; above 1 the output saturates "
                    f"instead of wrapping)")]
        return L

    k_sec = d.order
    L += [("h", "Transfer function: cascade of second-order sections"),
          ("m", r"$H(z) \;=\; \prod_{k=1}^{K}\dfrac{b_{0k} + b_{1k}z^{-1} + b_{2k}z^{-2}}"
                r"{1 + a_{1k}z^{-1} + a_{2k}z^{-2}}$" + rf"$\qquad K = {k_sec}$")]
    if d.type == Q.BIQUAD_Q31:
        L += [("h", "Difference equation per section  (Direct Form I, DSP task)"),
              ("m", r"$y[n] = b_0x[n] + b_1x[n-1] + b_2x[n-2] - a_1y[n-1] - a_2y[n-2]$")]
    else:
        L += [("h", "Difference equation per section  (Direct Form II Transposed, DSP task)"),
              ("m", r"$y[n] = b_0x[n] + s_1,\qquad s_1 \leftarrow b_1x[n] - a_1y[n] + s_2,"
                    r"\qquad s_2 \leftarrow b_2x[n] - a_2y[n]$")]

    L += [("h", "Design")]
    if spec.family == "notch":
        w0 = 2 * np.pi * p["f0"] / fs
        alpha = np.sin(w0) / (2 * p["q"])
        r = np.sqrt((1 - alpha) / (1 + alpha))
        L += [("m", rf"$\omega_0 = \dfrac{{2\pi f_0}}{{f_s}} = {w0:.5f},\qquad "
                    rf"\alpha = \dfrac{{\sin\omega_0}}{{2Q}} = {alpha:.5f}$" + rf"$\qquad f_0 = {p['f0']}$ Hz, $Q = {p['q']}$"),
              ("m", r"$H(z) = \dfrac{1 - 2\cos\omega_0\,z^{-1} + z^{-2}}"
                    r"{(1+\alpha) - 2\cos\omega_0\,z^{-1} + (1-\alpha)\,z^{-2}}$"),
              ("m", rf"zeros at $z = e^{{\pm j\omega_0}}$ (on the unit circle), poles at $r\,e^{{\pm j\omega_0}},\;"
                    rf"r = \sqrt{{\dfrac{{1-\alpha}}{{1+\alpha}}}} = {r:.5f}$,  bandwidth $f_0/Q = {p['f0'] / p['q']:g}$ Hz")]
    else:
        eps = np.sqrt(10 ** (p["rp"] / 10) - 1)
        wc = 2 * fs * np.tan(np.pi * p["fc"] / fs)
        if spec.family == "ellip":
            L += [("m", r"$|H(j\Omega)|^2 = \dfrac{1}{1 + \varepsilon^2 R_N^2(\Omega)}$"
                        rf"$\qquad$ elliptic, $N = {p['order']}$, $R_p = {p['rp']}$ dB ($\varepsilon = {eps:.4f}$),"
                        rf" $R_s = {p['rs']}$ dB")]
        else:
            L += [("m", r"$|H(j\Omega)|^2 = \dfrac{1}{1 + \varepsilon^2 T_N^2(\Omega_c/\Omega)}$"
                        rf"$\qquad$ Chebyshev I high-pass, $N = {p['order']}$, $R_p = {p['rp']}$ dB "
                        rf"($\varepsilon = {eps:.4f}$)")]
        L += [("m", r"$s = 2f_s\,\dfrac{1 - z^{-1}}{1 + z^{-1}}$  (bilinear)"
                    rf"$\qquad \Omega_c = 2f_s\tan\!\left(\dfrac{{\pi f_c}}{{f_s}}\right) = {wc:,.0f}$ rad/s"
                    rf"  (pre-warped, $f_c = {p['fc']}$ Hz)")]

    L += [("h", "Stability (checked on the board before a filter is accepted)"),
          ("m", r"poles of $1 + a_1z^{-1} + a_2z^{-2}$ inside $|z| = 1 \Leftrightarrow |a_2| < 1\;$ and $\;|a_1| < 1 + a_2$")]
    if d.type == Q.BIQUAD_Q31:
        s = d.post_shift
        L += [("h", f"On the board: Q31 data, Q{31 - s} coefficients (post_shift s = {s}), 64-bit accumulator"),
              ("m", rf"$c_{{int}} = \mathrm{{round}}\!\left(c\cdot 2^{{31-s}}\right)\qquad "
                    rf"y = \mathrm{{sat}}_{{32}}\!\left(\left(\mathrm{{acc}} + 2^{{30-s}}\right) \gg (31-s)\right)"
                    rf"\qquad \sum|c| < 2^{{1+s}} \Rightarrow |\mathrm{{acc}}| < 2^{{63}}$")]
    else:
        L += [("h", "On the board: IEEE-754 float32 on the Cortex-M4 FPU"),
              ("m", r"$x_f = x_{Q15}\cdot 2^{-15}\qquad y_{Q15} = \mathrm{sat}_{16}\!\left(\mathrm{round}"
                    r"(y_f\cdot 2^{15})\right)$")]
    L += [("h", "Section coefficients as they run on the board")]
    L += [("c", row) for row in _section_rows(d)]
    return L


def draw_lines(fig, title: str, subtitle: str, lines: list[tuple[str, str]]) -> None:
    """Lay lines out top-down using each line's *measured* rendered height
    (summation limits and fractions are much taller than prose), shrinking
    every font together until the page fits."""
    sizes = {"h": 11.5, "m": 14.0, "t": 10.0, "c": 9.5}
    gap = {"h": 0.006, "m": 0.016, "t": 0.010, "c": 0.004}
    for scale in (1.0, 0.92, 0.85, 0.78, 0.72, 0.66, 0.6, 0.55):
        fig.clear()
        fig.text(0.035, 0.965, title, fontsize=14, fontweight="bold", va="top")
        fig.text(0.035, 0.925, subtitle, fontsize=9.5, color=INK_2, va="top")
        renderer = fig.canvas.get_renderer()
        fig_h = fig.get_size_inches()[1] * fig.dpi
        y = 0.88
        for kind, text in lines:
            if kind == "h":
                y -= 0.014 * scale
            style = {"h": {"fontweight": "bold", "color": C_IN}, "m": {"color": INK},
                     "t": {"color": INK_2}, "c": {"family": "monospace", "color": INK}}[kind]
            x = 0.035 if kind == "h" else 0.06
            t = fig.text(x, y, text, fontsize=sizes[kind] * scale, va="top", **style)
            y -= t.get_window_extent(renderer).height / fig_h + gap[kind] * scale
        if y > 0.02:
            return


def draw_equations(fig, spec: FilterSpec, idx: int, total: int) -> None:
    draw_lines(fig, f"[{idx + 1}/{total}]  {spec.desc.name}",
               f"kernel: {Q.TYPE_NAMES[spec.desc.type]}    fs = {FS_HZ} Hz    see docs/DSP_MATH.md for derivations",
               equation_lines(spec))


# ------------------------------------------------------------------------------ app

class Viewer:
    def __init__(self, runs: list[Run], start: int):
        self.runs = runs
        self.idx = start % len(runs)
        self.sig = plt.figure("RTDSP-F4 - signals", figsize=(13, 7.5))
        self.eq = plt.figure("RTDSP-F4 - equations", figsize=(12.5, 9.5))
        self.buttons: list[Button] = []
        for fig in (self.sig, self.eq):
            fig.canvas.mpl_connect("key_press_event", self.on_key)
        self.render()

    def _add_buttons(self) -> None:
        prev_ax = self.sig.add_axes((0.70, 0.02, 0.13, 0.055))
        next_ax = self.sig.add_axes((0.845, 0.02, 0.13, 0.055))
        self.buttons = [Button(prev_ax, "<  Prev", color=GRID, hovercolor="#cde2fb"),
                        Button(next_ax, "Next  >", color=GRID, hovercolor="#cde2fb")]
        self.buttons[0].on_clicked(lambda _e: self.step(-1))
        self.buttons[1].on_clicked(lambda _e: self.step(+1))

    def render(self) -> None:
        run = self.runs[self.idx]
        draw_signals(self.sig, run, self.idx, len(self.runs))
        self._add_buttons()
        draw_equations(self.eq, run.spec, self.idx, len(self.runs))
        self.sig.canvas.draw_idle()
        self.eq.canvas.draw_idle()

    def step(self, delta: int) -> None:
        self.idx = (self.idx + delta) % len(self.runs)
        self.render()

    def on_key(self, event) -> None:
        if event.key in ("right", "n", " "):
            self.step(+1)
        elif event.key in ("left", "p"):
            self.step(-1)


def main() -> int:
    args = ARGS.parse_args()
    specs = catalog.demo_specs()
    runs = collect(specs, args.source)
    if args.save:
        args.save.mkdir(parents=True, exist_ok=True)
        sig = plt.figure(figsize=(13, 7.5))
        eq = plt.figure(figsize=(12.5, 9.5))
        for i, run in enumerate(runs):
            draw_signals(sig, run, i, len(runs))
            sig.savefig(args.save / f"signals_{i}.png", dpi=110)
            draw_equations(eq, run.spec, i, len(runs))
            eq.savefig(args.save / f"equations_{i}.png", dpi=110)
        print(f"wrote {2 * len(runs)} PNGs to {args.save}")
        return 0
    Viewer(runs, args.filter)
    plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
