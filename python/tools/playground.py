#!/usr/bin/env python3
"""Adjustable-filter playground running on the emulated STM32.

A clean sine wave plus adjustable white noise goes in. Sliders and buttons
set the filter: low-pass / high-pass / gain-only, IIR (Butterworth, Q31
biquads) or FIR (windowed sinc, Q15), cutoff, order and gain. Every change
is redesigned, quantised, uploaded to the board, and the board's own output
is plotted with its spectrum and the filter's frequency response.

    python python/tools/playground.py                  # live, on the emulated board
    python python/tools/playground.py --source model   # Python bit-exact model, no emulator
    python python/tools/playground.py --save build/play  # render preset states to PNG

Tip: click on the frequency-response plot to move the cutoff there.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

ARGS = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ARGS.add_argument("--source", choices=("board", "model"), default="board",
                  help="board: every change runs on the QEMU STM32 (default); model: Python bit-exact model")
ARGS.add_argument("--save", type=Path, help="render a few preset settings to PNGs in this directory, no window")

if __name__ == "__main__" and "--save" in sys.argv:
    import matplotlib

    matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.widgets import Button, RadioButtons, Slider  # noqa: E402
from scipy import signal  # noqa: E402

import visualize as V  # noqa: E402  (shared palette, style and equation renderer)
from rtdsp import FS_HZ, analysis, design, fixedpoint  # noqa: E402
from rtdsp import quantize as Q  # noqa: E402
from rtdsp.catalog import FilterSpec  # noqa: E402

C_IN, C_OUT, INK, INK_2, GRID, SURFACE = V.C_IN, V.C_OUT, V.INK, V.INK_2, V.GRID, V.SURFACE
C_WARN = "#d03b3b"            # status: critical (always paired with a text label)
N = 4096                      # samples per run (85 ms)
STEADY = 2048                 # metrics and spectra use the last 2048 samples (after the transient)
RESPONSES = ("Low-pass", "High-pass", "Gain only")
KERNELS = ("IIR  Q31", "FIR  Q15")


@dataclass(frozen=True)
class Settings:
    f_sine: float = 1000.0
    amp: float = 0.5
    noise: float = 0.15
    response: str = "Low-pass"
    kernel: str = "IIR  Q31"
    cutoff: float = 2000.0
    order: int = 4
    gain_db: float = 0.0


# --------------------------------------------------------------------- signal + design

def make_input(s: Settings) -> tuple[np.ndarray, np.ndarray]:
    """(x_q15, clean sine in full-scale units). The noise seed is fixed so that
    moving a filter slider changes only the filter, never the input."""
    t = np.arange(N) / FS_HZ
    clean = s.amp * np.sin(2 * np.pi * s.f_sine * t)
    noise = s.noise * np.random.default_rng(7).uniform(-1.0, 1.0, N)
    x = np.clip(np.round((clean + noise) * 32768), -32768, 32767).astype(np.int16)
    return x, clean


def design_filter(s: Settings) -> tuple[Q.FilterDesc, list[str], list[str]]:
    """Settings -> board descriptor. The gain is folded into the numerator
    (no extra multiply on the board). Returns (desc, info lines, warnings)."""
    g = 10 ** (s.gain_db / 20)
    fc = float(np.clip(s.cutoff, 20.0, 0.45 * FS_HZ))
    info, warn = [], []

    if s.response == "Gain only":
        desc = Q.biquad_q31(np.array([[g, 0, 0, 1, 0, 0]]), label=f"gain {s.gain_db:+.1f} dB")
        info.append(f"single section y = {g:.3f}·x  (Q31, post_shift {desc.post_shift})")
        return desc, info, warn

    btype = "lowpass" if s.response == "Low-pass" else "highpass"
    if s.kernel.startswith("IIR"):
        sos = design.butter_sos(s.order, fc, btype=btype)
        sos[0, :3] *= g
        if len(sos) > 1:
            sos = Q.scale_sos_linf(sos)
        desc = Q.biquad_q31(sos, label=f"Butterworth {btype}, order {s.order}, {fc:.0f} Hz")
        r = analysis.pole_radii(Q.quantised_sos(desc)).max()
        info.append(f"Butterworth order {s.order} -> {desc.order} Q31 biquad section(s), post_shift {desc.post_shift}, "
                    f"max |pole| = {r:.5f}")
    else:
        taps = min(16 * s.order + 1, 127)
        h = signal.firwin(taps, fc, pass_zero=btype, window="hamming", fs=FS_HZ) * g
        peak = np.max(np.abs(h))
        if peak >= 32767 / 32768:
            h *= (32767 / 32768) / peak
            warn.append(f"gain limited to {20 * np.log10(g * (32767 / 32768) / peak):+.1f} dB: "
                        f"a larger FIR tap does not fit Q15")
        desc = Q.fir_q15(h, label=f"FIR {btype}, {taps} taps, {fc:.0f} Hz")
        info.append(f"Hamming-windowed sinc, {taps} taps, Q15, group delay {(taps - 1) // 2} samples "
                    f"({(taps - 1) / 2 / FS_HZ * 1e3:.2f} ms), sum|h| = {Q.fir_l1_gain(h):.2f}")
    return desc, info, warn


def sine_fit(v: np.ndarray, f: float) -> tuple[float, float]:
    """Least-squares fit of A sin + B cos at the known frequency over the steady
    part: returns (amplitude, SNR in dB). The residual is everything that is not
    the sine (noise, plus distortion when the output clips)."""
    seg = v[-STEADY:].astype(float) / 32768
    n = np.arange(STEADY)
    basis = np.column_stack([np.sin(2 * np.pi * f / FS_HZ * n), np.cos(2 * np.pi * f / FS_HZ * n),
                             np.ones(STEADY)])
    coef, *_ = np.linalg.lstsq(basis, seg, rcond=None)
    resid = seg - basis @ coef
    amp = float(np.hypot(coef[0], coef[1]))
    p_noise = float(np.mean(resid ** 2))
    snr = 10 * np.log10((amp * amp / 2) / max(p_noise, 1e-15)) if amp > 0 else -np.inf
    return amp, snr


# --------------------------------------------------------------------------- engine

class Engine:
    """Runs a descriptor on the emulated board (or the model) and checks bit-exactness."""

    def __init__(self, source: str):
        self.source = source
        self.board = None
        self.dev = None
        self._loaded: bytes | None = None
        if source == "board":
            from rtdsp.emulator import QemuBoard, find_qemu  # noqa: PLC0415
            if find_qemu() is None:
                print("qemu-system-arm not found: using the Python bit-exact model")
                self.source = "model"
            else:
                self.board = QemuBoard()
                self.dev = self.board.start()
                info = self.dev.wait_boot(10)
                print(f"emulated STM32 booted (fw {info.fw_version}, POST {'PASS' if info.post_result == 0 else 'FAIL'})")

    @property
    def label(self) -> str:
        return "emulated STM32F405" if self.source == "board" else "Python bit-exact model"

    def run(self, desc: Q.FilterDesc, x: np.ndarray) -> tuple[np.ndarray, bool]:
        model = fixedpoint.engine(desc, x)
        if self.dev is None:
            return model, True
        raw = desc.to_bytes()
        if raw != self._loaded:
            self.dev.set_filter(raw)   # validated on the board, swapped at a block boundary
            self._loaded = raw
        self.dev.reset_state()         # same start state as the model
        y = self.dev.process(x, window=6)
        return y, bool(np.array_equal(y, model))

    def close(self) -> None:
        if self.board:
            self.board.stop()
            self.board = self.dev = None


# ------------------------------------------------------------------------------- UI

def spectrum(v: np.ndarray):
    seg = v[-STEADY:].astype(float) / 32768
    w = np.hanning(STEADY)
    mag = np.abs(np.fft.rfft(seg * w)) * 2 / np.sum(w)
    return np.fft.rfftfreq(STEADY, 1 / FS_HZ), 20 * np.log10(np.maximum(mag, 1e-7))


class Playground:
    def __init__(self, engine: Engine, settings: Settings = Settings(), interactive: bool = True):
        self.engine = engine
        self.s = settings
        self.interactive = interactive
        self.eq_fig = None
        self.fig = plt.figure("RTDSP-F4 - filter playground", figsize=(15, 9.8))
        self._build_axes()
        self._build_controls()
        self.timer = self.fig.canvas.new_timer(interval=160)
        self.timer.single_shot = True
        self.timer.add_callback(self.apply)
        self.fig.canvas.mpl_connect("button_press_event", self._on_click)
        self.fig.canvas.mpl_connect("close_event", lambda _e: self._on_close())
        self.apply()

    # ---- layout
    def _build_axes(self) -> None:
        f = self.fig
        gs = f.add_gridspec(3, 2, width_ratios=[1.6, 1], height_ratios=[1, 1, 1.15], left=0.06, right=0.975,
                            top=0.835, bottom=0.40, hspace=0.62, wspace=0.16)
        self.ax_x = f.add_subplot(gs[0, 0])
        self.ax_y = f.add_subplot(gs[1, 0], sharex=self.ax_x, sharey=self.ax_x)
        self.ax_X = f.add_subplot(gs[0, 1])
        self.ax_Y = f.add_subplot(gs[1, 1], sharex=self.ax_X, sharey=self.ax_X)
        self.ax_h = f.add_subplot(gs[2, :])

        (self.l_x,) = self.ax_x.plot([], [], color=C_IN, linewidth=1.3)
        (self.l_ref,) = self.ax_y.plot([], [], color=INK_2, linewidth=1.0, linestyle=(0, (4, 3)),
                                       label="clean input sine (reference)")
        (self.l_y,) = self.ax_y.plot([], [], color=C_OUT, linewidth=1.6, label="board output")
        self.ax_y.legend(loc="upper right", fontsize=8.5, ncol=2)
        self.ax_x.set_ylim(-1.15, 1.15)
        self.ax_x.set_ylabel("x[n] (FS)")
        self.ax_y.set_ylabel("y[n] (FS)")
        self.ax_y.set_xlabel("time (ms)")

        (self.l_X,) = self.ax_X.semilogx([], [], color=C_IN, linewidth=1.0)
        (self.l_Y,) = self.ax_Y.semilogx([], [], color=C_OUT, linewidth=1.0)
        self.ax_X.set_xlim(20, FS_HZ / 2)
        self.ax_X.set_ylim(-110, 5)
        self.ax_X.set_ylabel("dBFS")
        self.ax_Y.set_ylabel("dBFS")
        self.ax_Y.set_xlabel("frequency (Hz)")
        self.ax_X.set_title("input spectrum", loc="left", fontsize=10, color=INK_2)
        self.ax_Y.set_title("output spectrum", loc="left", fontsize=10, color=INK_2)

        (self.l_h,) = self.ax_h.semilogx([], [], color=C_OUT, linewidth=2.0)
        self.v_sine = self.ax_h.axvline(1000, color=C_IN, linewidth=1.2, linestyle=(0, (2, 2)))
        self.v_cut = self.ax_h.axvline(1000, color=INK_2, linewidth=1.0, linestyle=(0, (5, 3)))
        self.h_note = self.ax_h.annotate("", (1000, 0), xytext=(8, 10), textcoords="offset points",
                                         fontsize=9, color=INK,
                                         bbox={"boxstyle": "round,pad=0.25", "fc": SURFACE, "ec": GRID})
        self.ax_h.set_xlim(20, FS_HZ / 2)
        self.ax_h.set_ylim(-80, 26)
        self.ax_h.set_xlabel("frequency (Hz)")
        self.ax_h.set_ylabel("|H(f)| (dB)")
        self.ax_h.set_title("frequency response of the quantised filter on the board   "
                            "(click here to move the cutoff)", loc="left", fontsize=10.5, color=INK)

        self.t_title = f.text(0.06, 0.975, "", fontsize=13, fontweight="bold", va="top")
        self.t_metrics = f.text(0.06, 0.94, "", fontsize=10, color=INK, va="top")
        self.t_info = f.text(0.06, 0.915, "", fontsize=9, color=INK_2, va="top")
        self.t_warn = f.text(0.06, 0.893, "", fontsize=9, color=C_WARN, va="top")

    def _slider(self, rect, label, vmin, vmax, vinit, step=None, fmt=None):
        ax = self.fig.add_axes(rect)
        sl = Slider(ax, label, vmin, vmax, valinit=vinit, valstep=step, color=C_IN, initcolor="none")
        sl.label.set_fontsize(9.5)
        sl.valtext.set_fontsize(9.5)
        if fmt:
            sl.valtext.set_text(fmt(vinit))
            sl.on_changed(lambda v, sl=sl: sl.valtext.set_text(fmt(v)))
        sl.on_changed(lambda _v: self.schedule())
        return sl

    def _build_controls(self) -> None:
        f, s = self.fig, self.s
        hz = lambda lv: V.khz(round(10 ** lv))  # noqa: E731
        f.text(0.09, 0.315, "INPUT SIGNAL", fontsize=10, fontweight="bold", color=C_IN)
        f.text(0.47, 0.315, "FILTER", fontsize=10, fontweight="bold", color=C_OUT)
        self.sl_f = self._slider((0.09, 0.27, 0.25, 0.026), "sine freq", np.log10(20), np.log10(20000),
                                 np.log10(s.f_sine), fmt=hz)
        self.sl_a = self._slider((0.09, 0.225, 0.25, 0.026), "amplitude", 0.0, 1.0, s.amp,
                                 fmt=lambda v: f"{v:.2f} FS")
        self.sl_n = self._slider((0.09, 0.18, 0.25, 0.026), "noise", 0.0, 0.6, s.noise,
                                 fmt=lambda v: f"{v:.2f} FS")
        self.sl_c = self._slider((0.47, 0.27, 0.25, 0.026), "cutoff", np.log10(20), np.log10(0.45 * FS_HZ),
                                 np.log10(s.cutoff), fmt=hz)
        self.sl_o = self._slider((0.47, 0.225, 0.25, 0.026), "order", 1, 8, s.order, step=1,
                                 fmt=lambda v: f"{int(v)}")
        self.sl_g = self._slider((0.47, 0.18, 0.25, 0.026), "gain", -20.0, 20.0, s.gain_db, step=0.5,
                                 fmt=lambda v: f"{v:+.1f} dB")

        f.text(0.795, 0.315, "response", fontsize=9.5, color=INK_2)
        f.text(0.895, 0.315, "kernel", fontsize=9.5, color=INK_2)
        self.rb_resp = RadioButtons(f.add_axes((0.79, 0.165, 0.095, 0.14), frameon=False), RESPONSES,
                                    active=RESPONSES.index(s.response), activecolor=C_OUT)
        self.rb_kern = RadioButtons(f.add_axes((0.89, 0.19, 0.095, 0.115), frameon=False), KERNELS,
                                    active=KERNELS.index(s.kernel), activecolor=C_OUT)
        for rb in (self.rb_resp, self.rb_kern):
            for lbl in rb.labels:
                lbl.set_fontsize(9.5)
            rb.on_clicked(lambda _l: self.schedule())

        self.bt_reset = Button(f.add_axes((0.79, 0.075, 0.09, 0.045)), "Reset", color=GRID, hovercolor="#cde2fb")
        self.bt_eq = Button(f.add_axes((0.89, 0.075, 0.09, 0.045)), "Equations", color=GRID, hovercolor="#cde2fb")
        self.bt_reset.on_clicked(lambda _e: self.reset())
        self.bt_eq.on_clicked(lambda _e: self.open_equations())
        f.text(0.09, 0.105, "Every change is redesigned in Python, quantised, validated and uploaded to the board,\n"
                            "which filters the signal; the plots show what the board sent back.",
               fontsize=9, color=INK_2, va="top", linespacing=1.5)

    # ---- state
    def read_controls(self) -> Settings:
        return Settings(f_sine=10 ** self.sl_f.val, amp=self.sl_a.val, noise=self.sl_n.val,
                        response=self.rb_resp.value_selected, kernel=self.rb_kern.value_selected,
                        cutoff=10 ** self.sl_c.val, order=int(self.sl_o.val), gain_db=self.sl_g.val)

    def schedule(self) -> None:
        """Debounce: dragging a slider fires many events; run the board once it settles."""
        if not self.interactive:
            return
        self.timer.stop()
        self.timer.start()

    def reset(self) -> None:
        self.interactive, was = False, self.interactive
        d = Settings()
        self.sl_f.set_val(np.log10(d.f_sine))
        self.sl_a.set_val(d.amp)
        self.sl_n.set_val(d.noise)
        self.sl_c.set_val(np.log10(d.cutoff))
        self.sl_o.set_val(d.order)
        self.sl_g.set_val(d.gain_db)
        self.rb_resp.set_active(RESPONSES.index(d.response))
        self.rb_kern.set_active(KERNELS.index(d.kernel))
        self.interactive = was
        self.apply()

    def _on_click(self, event) -> None:
        if event.inaxes is self.ax_h and event.xdata and self.rb_resp.value_selected != "Gain only":
            self.sl_c.set_val(float(np.clip(np.log10(event.xdata), self.sl_c.valmin, self.sl_c.valmax)))

    def _on_close(self) -> None:
        self.engine.close()
        if self.eq_fig is not None:
            plt.close(self.eq_fig)

    # ---- the work
    def apply(self) -> None:
        s = self.read_controls()
        self.s = s
        x, clean = make_input(s)
        desc, info, warn = design_filter(s)
        t0 = time.monotonic()
        y, exact = self.engine.run(desc, x)
        dt = time.monotonic() - t0
        self.desc = desc
        self._draw(s, x, clean, y, desc, info, warn, exact, dt)
        if self.eq_fig is not None:
            self._draw_equations()

    def _draw(self, s, x, clean, y, desc, info, warn, exact, dt) -> None:
        span = float(np.clip(5.0 / s.f_sine, 0.002, 0.040))            # ~5 periods on screen
        n_view = int(span * FS_HZ)
        idx = np.arange(N - n_view, N)
        t_ms = idx / FS_HZ * 1e3
        self.l_x.set_data(t_ms, x[idx] / 32768)
        self.l_y.set_data(t_ms, y[idx] / 32768)
        self.l_ref.set_data(t_ms, clean[idx])
        self.ax_x.set_xlim(t_ms[0], t_ms[-1])

        fX, dX = spectrum(x)
        fY, dY = spectrum(y)
        self.l_X.set_data(fX[1:], dX[1:])
        self.l_Y.set_data(fY[1:], dY[1:])

        f, h = analysis.response(desc, n=4096)
        self.l_h.set_data(f[1:], analysis.mag_db(h[1:]))
        h_sine = 20 * np.log10(max(abs(np.interp(s.f_sine, f, np.abs(h))), 1e-12))
        self.v_sine.set_xdata([s.f_sine, s.f_sine])
        self.v_cut.set_xdata([s.cutoff, s.cutoff])
        self.v_cut.set_visible(s.response != "Gain only")
        self.h_note.xy = (s.f_sine, float(np.clip(h_sine, -78, 24)))
        self.h_note.set_text(f"sine {V.khz(round(s.f_sine))}: |H| = {h_sine:+.1f} dB")

        a_in, snr_in = sine_fit(x, s.f_sine)
        a_out, snr_out = sine_fit(y, s.f_sine)
        clipped = int(np.sum((y == 32767) | (y == -32768)))
        gain_meas = 20 * np.log10(a_out / a_in) if a_in > 1e-6 and a_out > 1e-9 else -np.inf
        self.t_title.set_text(f"{s.response}  ·  {desc.name}  ·  running on {self.engine.label}  "
                              f"({'bit-exact vs model' if exact else 'MISMATCH vs model'}, update {dt * 1e3:.0f} ms)")
        self.t_metrics.set_text(
            f"sine amplitude: in {a_in:.3f} FS -> out {a_out:.3f} FS  ({gain_meas:+.1f} dB)        "
            f"SNR: in {snr_in:.1f} dB -> out {snr_out:.1f} dB  ({snr_out - snr_in:+.1f} dB)        "
            f"clipped samples: {clipped}")
        if clipped:
            warn = warn + [f"output saturates on {clipped} samples: lower the gain or the amplitude "
                           f"(the board clips instead of wrapping)"]
        self.t_info.set_text("   ".join(info))
        self.t_warn.set_text(("WARNING: " + "   ".join(warn)) if warn else "")
        self.ax_x.set_title(f"INPUT  -  {V.khz(round(s.f_sine))} sine, amplitude {s.amp:.2f} + noise {s.noise:.2f}",
                            loc="left", fontsize=10.5, color=INK)
        self.ax_y.set_title("OUTPUT  -  from the board", loc="left", fontsize=10.5, color=INK)
        self.fig.canvas.draw_idle()

    # ---- equations window
    def open_equations(self) -> None:
        if self.eq_fig is None:
            self.eq_fig = plt.figure("RTDSP-F4 - current filter equations", figsize=(12.5, 8))
            self.eq_fig.canvas.mpl_connect("close_event", lambda _e: setattr(self, "eq_fig", None))
        self._draw_equations()
        if self.interactive:
            self.eq_fig.show()

    def _draw_equations(self) -> None:
        d = self.desc
        spec = FilterSpec(d, np.zeros(0), "", {}, ())
        lines = V.exact_tf_lines(spec)
        if d.type == Q.BIQUAD_Q31:
            lines += [("h", "Sections as they run on the board (stability triangle checked on upload)")]
            lines += [("c", r) for r in V._section_rows(d)]
        where = "every FIR tap" if d.type == Q.FIR_Q15 else "the numerator (b0, b1, b2) of the first section"
        lines += [("h", "Gain"),
                  ("t", f"{self.s.gain_db:+.1f} dB = x{10 ** (self.s.gain_db / 20):.3f}, folded into {where}, "
                        "so the board spends no extra multiply on it")]
        V.draw_lines(self.eq_fig, f"Current filter: {d.name}",
                     f"kernel {Q.TYPE_NAMES[d.type]}, fs = {FS_HZ} Hz, updates live with the sliders", lines)
        self.eq_fig.canvas.draw_idle()


# ----------------------------------------------------------------------------- main

PRESETS = {
    "lowpass_noise": Settings(),
    "highpass": Settings(f_sine=6000, amp=0.5, noise=0.2, response="High-pass", cutoff=3000, order=6),
    "gain_clipping": Settings(f_sine=500, amp=0.6, noise=0.05, cutoff=1500, order=2, gain_db=8.0),
    "fir_lowpass": Settings(f_sine=800, amp=0.5, noise=0.3, kernel="FIR  Q15", cutoff=1500, order=6),
    "attenuate": Settings(f_sine=2000, amp=0.8, noise=0.0, response="Gain only", gain_db=-12.0),
}


def main() -> int:
    args = ARGS.parse_args()
    engine = Engine(args.source)
    try:
        if args.save:
            args.save.mkdir(parents=True, exist_ok=True)
            for name, preset in PRESETS.items():
                pg = Playground(engine, preset, interactive=False)
                pg.fig.savefig(args.save / f"play_{name}.png", dpi=100)
                pg.open_equations()
                pg.eq_fig.savefig(args.save / f"play_{name}_eq.png", dpi=100)
                plt.close("all")
                print(f"{name}: {pg.t_metrics.get_text()}")
            return 0
        Playground(engine)
        plt.show()
    finally:
        engine.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
