#!/usr/bin/env python3
"""Render the Bhangmeter V3 documentation figures.

Every figure is written twice -- ``<name>-dark.png`` and ``<name>-light.png`` --
so the top-level README can serve a theme-matched image via a GitHub
``<picture>`` + ``prefers-color-scheme`` block.

Two independent data sources feed the figures:

* **Timing / yield / waveform figures** use the *firmware* scaling constants
  verbatim (see ``code/optical_bhang/optical_bhang_runtime.c``), which now follow
  the published literature (Glasstone & Dolan 1977 §7.85/§7.86/§2.125; Nuclear
  Weapon Archive FAQ §5).  This is what the device actually computes, so the
  figures and the README table agree with the instrument.

      t_inter (peak->peak) = 41.7 * W^0.44   ms   [G&D §7.85]
      t_min   (peak->min)  =  1.87 * W^0.576 ms   [fit: G&D §2.125 + NWA FAQ]
      W = (t_inter / 41.7)^(1/0.44)               (1/0.44   ~= 2.273)
      W = (t_min   / 1.87)^(1/0.576)              (1/0.576  ~= 1.736)

  Note the two times scale with *different* exponents: t_min/t_inter is not a
  fixed 1/10 ratio (it runs ~0.06 at 10 kt to ~0.115 at 1 Mt).

  The double-pulse waveform is no longer an illustrative log-normal tuned to the
  firmware; it is fitted to the G&D §7.85/§7.86 power-and-energy anchors and the
  NWA FAQ minimum-depth/timing anchors (the inter-pulse minimum sits ~0.1 of the
  second-peak power — a ten-fold, ~1-decade dip, never dark).

* **Photocurrent / VLOG figures** reproduce the AD8304 + VEMD2704 optical
  front-end design report.  The report's photocurrent-vs-yield curves are clean
  power laws (the 1st-pulse exponent 0.70 / 2nd-pulse 0.56 follow Glasstone &
  Dolan's P_max ~ W^0.56), so they are reconstructed here from a handful of
  calibrated coefficients rather than copied as raw arrays.  Spot-checks against
  the report's table reproduce it to the quoted precision, e.g.
  100 kt / 5 km / 2nd pulse -> 6.40e-4 A.

Run:  python assets/src/generate_figures.py
"""
from __future__ import annotations

import os

import matplotlib

matplotlib.use("Agg")  # headless: write files, never open a window

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import to_hex
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

# --------------------------------------------------------------------------- #
# Output location: repo_root/assets/  (this file lives in repo_root/assets/src)
# --------------------------------------------------------------------------- #
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.abspath(os.path.join(HERE, ".."))

DPI = 120

# --------------------------------------------------------------------------- #
# Firmware scaling constants (mirror optical_bhang_runtime.c)
#   t_inter (peak->peak) = 41.7  * W^0.44   ms   [G&D §7.85]
#   t_min   (peak->min)  =  1.87 * W^0.576  ms   [G&D §2.125 + NWA FAQ fit]
# The two scale with different exponents — the old fixed 1/10 ratio does not hold.
# --------------------------------------------------------------------------- #
K_TIME_INTER_MS = 41.7
K_TIME_INTER_EXP = 0.44
K_YIELD_EXP_INTER = 1.0 / K_TIME_INTER_EXP  # ~= 2.273
K_TIME_MIN_MS = 1.87
K_TIME_MIN_EXP = 0.576
K_YIELD_EXP_MIN = 1.0 / K_TIME_MIN_EXP  # ~= 1.736

# Firmware validity windows (10 kt - 1 Mt with slack)
T_INTER_MIN_MS, T_INTER_MAX_MS = 90.0, 1050.0
T_MIN_MIN_MS, T_MIN_MAX_MS = 5.0, 130.0

PRESETS_KT = [10, 50, 100, 250, 500, 1000]


def t_inter_ms(W_kt):
    return K_TIME_INTER_MS * np.power(W_kt, K_TIME_INTER_EXP)


def t_min_ms(W_kt):
    return K_TIME_MIN_MS * np.power(W_kt, K_TIME_MIN_EXP)


def W_from_inter(t_ms):
    return np.power(t_ms / K_TIME_INTER_MS, K_YIELD_EXP_INTER)


def W_from_min(t_ms):
    return np.power(t_ms / K_TIME_MIN_MS, K_YIELD_EXP_MIN)


# --------------------------------------------------------------------------- #
# AD8304 logarithmic transimpedance amp + VEMD2704 front-end
# --------------------------------------------------------------------------- #
# VLOG slope (V/decade) and intercept, from the BHG-3000 network at AD8304
# datasheet nominals: 200 mV/decade x 24.9k/(24.9k + 5k) [R2 from VREF against
# the 5 kohm VLOG source] x (1 + 6.34k/13.3k) [buffer gain, R4/R3] = 246 mV/decade;
# the 2 V VREF through R2 adds ~0.49 V at the output, which moves the intercept
# from 100 pA to ~1 pA. (R1, 309 kohm, is not fitted.)
VLOG_SLOPE = 0.246  # V / decade
I_REF = 1e-12  # 1 pA intercept
AD8304_I_MIN = 1e-10  # 100 pA floor
AD8304_I_MAX = 1e-2  # 10 mA ceiling
ADC_VREF = 2.5  # AD7091R internal 2.5 V reference
VLOG_CEILING = 2.8  # VPOS - 0.5 V


def vlog_from_current(i_a):
    """AD8304 transfer:  VLOG = 0.246 * log10(I / 1 pA)."""
    return VLOG_SLOPE * np.log10(np.asarray(i_a) / I_REF)


def current_from_vlog(v):
    return I_REF * np.power(10.0, np.asarray(v) / VLOG_SLOPE)


# Photocurrent-vs-yield reconstruction (standard atmosphere), calibrated to the
# design report.  I = C(d) * W^exp.  Coefficients are I at W = 1 kt.
PC_EXP = {"1st": 0.700, "2nd": 0.560}
PC_COEFF = {  # distance_km -> {pulse -> I@1kt}
    5: {"2nd": 4.855e-5, "1st": 2.3304e-5},
    10: {"2nd": 4.4652e-6, "1st": 2.1433e-6},
    20: {"2nd": 1.5107e-7, "1st": 7.2516e-8},
}
DIST_KM = [5, 10, 20]


def photocurrent(W_kt, dist_km, pulse):
    return PC_COEFF[dist_km][pulse] * np.power(W_kt, PC_EXP[pulse])


# --------------------------------------------------------------------------- #
# Literature double-pulse optical-power model.
#
# Fitted to the published power-and-energy and minimum-depth/timing anchors,
# replacing the old illustrative log-normal:
#   [G&D §7.85]  t_2max = 41.7*W^0.44 ms; P/Pmax(1.56 t_max)=0.59; E/Etot(1.56)=40%
#   [G&D §7.86]  E/Etot(10 t_max) = 80%
#   [G&D §7.03]  first pulse carries ~1% of radiant energy
#   [G&D §2.125] minimum ~11 ms for 20 kt
#   [NWA FAQ §5] power drops ~10x into the minimum; 2nd peak regains much of the
#                1st-peak luminosity (so the dip is ~1 decade, never dark).
#
# Second-pulse shape p2(tau), tau = t/t_2max (peak-normalised to 1); first pulse
# rises as (t/1ms)^3 with a power-law decay set so the total at t_min is
# min_depth * P_2max and the first pulse carries ~1% of the energy.
# --------------------------------------------------------------------------- #
P2X = np.array([1.063, 4.983, 0.022, 2.331, 3.399])


def _p2_raw(tau):
    c1, n1, s, c2, n2 = P2X
    return tau ** 2 / (1 + (tau / c1) ** n1) + s * tau ** 2 / (1 + (tau / c2) ** n2)


_P2_TG = np.geomspace(1e-3, 200, 100000)
_P2_PKV = _p2_raw(_P2_TG).max()


def p2(tau):
    """Second-pulse optical power vs tau = t / t_2max, normalised to peak = 1."""
    return _p2_raw(np.maximum(tau, 1e-9)) / _P2_PKV


def waveform_norm(t_ms, W_kt, r1=1.0, min_depth=0.10):
    """Total optical power, normalised to P_2max = 1.  r1 = P_1max / P_2max."""
    t2, tm, t1 = t_inter_ms(W_kt), t_min_ms(W_kt), 1.0
    pp2 = p2(t_ms / t2)
    target = max(min_depth - p2(np.array([tm / t2]))[0], 1e-3)
    q = np.log(r1 / target) / np.log(tm / t1)
    p1 = np.where(t_ms < t1,
                  r1 * np.clip(t_ms / t1, 0, None) ** 3,
                  r1 * (np.maximum(t_ms, t1) / t1) ** (-q))
    return p1 + pp2


def double_pulse_power(t_ms, W_kt):
    """Total normalised double-pulse optical power (P_2max = 1) at yield W_kt."""
    return waveform_norm(np.asarray(t_ms, dtype=float), W_kt)


# --------------------------------------------------------------------------- #
# Theming
# --------------------------------------------------------------------------- #
THEMES = {
    "dark": {
        "figure.facecolor": "#0c0e12",
        "savefig.facecolor": "#0c0e12",
        "axes.facecolor": "#141820",
        "axes.edgecolor": "#3a4357",
        "axes.labelcolor": "#dfe4ee",
        "text.color": "#dfe4ee",
        "xtick.color": "#9aa3b2",
        "ytick.color": "#9aa3b2",
        "grid.color": "#28303f",
        "legend.facecolor": "#141820",
        "legend.edgecolor": "#3a4357",
    },
    "light": {
        "figure.facecolor": "#ffffff",
        "savefig.facecolor": "#ffffff",
        "axes.facecolor": "#ffffff",
        "axes.edgecolor": "#9aa0aa",
        "axes.labelcolor": "#1b1f27",
        "text.color": "#1b1f27",
        "xtick.color": "#454b56",
        "ytick.color": "#454b56",
        "grid.color": "#d7dbe2",
        "legend.facecolor": "#ffffff",
        "legend.edgecolor": "#c2c7d0",
    },
}

BASE_RC = {
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.titleweight": "bold",
    "axes.labelsize": 11,
    "axes.grid": True,
    "grid.linewidth": 0.6,
    "grid.alpha": 0.7,
    "legend.framealpha": 0.85,
    "legend.fontsize": 9.5,
    "lines.linewidth": 2.0,
    "figure.autolayout": False,
}

# Theme-independent line colours (saturated -> legible on both backgrounds).
YIELD_COLORS = ["#ffd166", "#ffb454", "#ff8c42", "#ef5b25", "#d63a1f", "#a01717"]
DIST_COLORS = {5: "#3b82f6", 10: "#10b981", 20: "#f59e0b"}
C_TMIN, C_TINTER, C_DT = "#3b82f6", "#10b981", "#a855f7"
C_LIMIT = "#ef4444"
C_ACCENT = "#f59e0b"


def _render(name, draw):
    """Call ``draw(fig)`` under each theme and save ``<name>-<theme>.png``."""
    for theme, rc in THEMES.items():
        with plt.rc_context({**BASE_RC, **rc}):
            fig = plt.figure()
            draw(fig)
            path = os.path.join(OUT_DIR, f"{name}-{theme}.png")
            fig.savefig(path, dpi=DPI, bbox_inches="tight")
            plt.close(fig)
            print(f"  wrote {os.path.relpath(path, OUT_DIR)}")


def _yield_axis(ax):
    ax.set_xscale("log")
    ax.set_xlim(1, 1000)
    ax.set_xticks([1, 10, 100, 1000])
    ax.set_xticklabels(["1", "10", "100", "1000"])
    ax.set_xlabel("Yield (kt)")


# --------------------------------------------------------------------------- #
# Figure 1 - double-pulse overlay (normalised optical power vs log time)
# --------------------------------------------------------------------------- #
def fig_double_pulse_overlay(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.6, 5.0)
    t = np.logspace(np.log10(0.3), np.log10(3000), 1400)  # ms
    for W, c in zip(PRESETS_KT, YIELD_COLORS):
        p = double_pulse_power(t, W)
        label = f"{W} kt" if W < 1000 else "1 Mt"
        ax.plot(t, p / p.max(), color=c, label=label, linewidth=1.8)
    ax.set_xscale("log")
    ax.set_xlim(0.3, 3000)
    ax.set_ylim(0, 1.08)
    ax.set_xlabel("Time after detonation (ms, log scale)")
    ax.set_ylabel("Optical power (normalised)")
    ax.set_title("Double-pulse optical signature, 10 kt - 1 Mt")
    ax.legend(ncol=2, loc="upper right")
    ax.annotate("1st pulse\n(~1 ms, yield-independent)", xy=(1.0, 1.0),
                xytext=(3.0, 0.86), fontsize=8.5, color=C_TMIN,
                ha="left", va="center")
    ax.text(240, 0.69, "2nd pulse slides right\nwith yield  (t ~ W^0.44)",
            fontsize=8.5, color=C_TINTER, ha="center", va="center")


# --------------------------------------------------------------------------- #
# Figure 2 - annotated 100 kt double pulse with the three phases
# --------------------------------------------------------------------------- #
def fig_double_pulse_annotated(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.6, 5.0)
    W = 100
    tm, ti = float(t_min_ms(W)), float(t_inter_ms(W))
    t = np.logspace(np.log10(0.3), np.log10(3000), 1600)
    p = double_pulse_power(t, W)
    p /= p.max()
    ax.plot(t, p, color=C_ACCENT, linewidth=2.2, zorder=5)

    # Phase shading on the log-time axis.
    ax.axvspan(0.3, tm, color=C_TMIN, alpha=0.07)
    ax.axvspan(tm, ti, color=C_TINTER, alpha=0.07)
    ax.axvspan(ti, 3000, color=C_ACCENT, alpha=0.07)
    for x, txt, col in [(np.sqrt(0.3 * tm), "PHASE 1\nfireball", C_TMIN),
                        (np.sqrt(tm * ti), "PHASE 2\nshock / minimum", C_TINTER),
                        (np.sqrt(ti * 3000), "PHASE 3\nbreakaway", C_ACCENT)]:
        ax.text(x, 1.02, txt, ha="center", va="bottom", fontsize=8, color=col)

    for x, label, col in [(1.0, "1st peak\n~1 ms", C_TMIN),
                          (tm, f"minimum\nt_min = {tm:.0f} ms", C_TINTER),
                          (ti, f"2nd peak\nt_inter = {ti:.0f} ms", C_ACCENT)]:
        yi = float(np.interp(np.log10(x), np.log10(t), p))
        ax.plot([x], [yi], "o", color=col, zorder=6)
        ax.axvline(x, color=col, ls=":", lw=1.0, alpha=0.8)
        ax.annotate(label, xy=(x, yi), xytext=(x, yi - 0.22), fontsize=8.5,
                    color=col, ha="center", va="top")

    ax.set_xscale("log")
    ax.set_xlim(0.3, 3000)
    ax.set_ylim(0, 1.18)
    ax.set_xlabel("Time after detonation (ms, log scale)")
    ax.set_ylabel("Optical power (normalised)")
    ax.set_title("Anatomy of the 100 kt double pulse")


# --------------------------------------------------------------------------- #
# Figure 3 - timing vs yield
# --------------------------------------------------------------------------- #
def fig_timing_vs_yield(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.0, 5.2)
    W = np.logspace(0, 3, 400)
    ti, tm = t_inter_ms(W), t_min_ms(W)
    ax.plot(W, ti, color=C_TINTER, label="t_inter  (peak->peak)")
    ax.plot(W, tm, color=C_TMIN, label="t_min  (peak->min)")
    ax.plot(W, ti - tm, color=C_DT, ls="--", lw=1.7, label="dt  (t_inter - t_min)")
    # Firmware validity window.
    ax.axhspan(T_INTER_MIN_MS, T_INTER_MAX_MS, color=C_TINTER, alpha=0.06)
    for W0 in PRESETS_KT:
        ax.plot([W0], [t_inter_ms(W0)], "o", color=C_TINTER, ms=4, zorder=5)
        ax.plot([W0], [t_min_ms(W0)], "o", color=C_TMIN, ms=4, zorder=5)
    _yield_axis(ax)
    ax.set_yscale("log")
    ax.set_ylabel("Time (ms)")
    ax.set_title("Pulse timing vs yield  (t_inter = 41.7.W^0.44,  t_min = 1.87.W^0.576)")
    ax.legend(loc="upper left")


# --------------------------------------------------------------------------- #
# Figure 4 - photocurrent vs yield, with AD8304 floor/ceiling
# --------------------------------------------------------------------------- #
def fig_photocurrent_vs_yield(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.2, 5.4)
    W = np.logspace(0, 3, 400)
    for d in DIST_KM:
        c = DIST_COLORS[d]
        ax.plot(W, photocurrent(W, d, "2nd"), color=c, lw=2.0,
                label=f"2nd pulse @ {d} km")
        ax.plot(W, photocurrent(W, d, "1st"), color=c, lw=1.5, ls="--",
                label=f"1st pulse @ {d} km")
    ax.axhline(AD8304_I_MAX, color=C_LIMIT, ls=(0, (6, 3)), lw=1.4)
    ax.axhline(AD8304_I_MIN, color=C_LIMIT, ls=(0, (6, 3)), lw=1.4)
    ax.text(1.1, AD8304_I_MAX, " AD8304 ceiling (10 mA)", color=C_LIMIT,
            fontsize=8, va="bottom")
    ax.text(1.1, AD8304_I_MIN, " AD8304 floor (100 pA)", color=C_LIMIT,
            fontsize=8, va="bottom")
    _yield_axis(ax)
    ax.set_yscale("log")
    ax.set_ylim(3e-11, 3e-2)
    ax.set_ylabel("Photocurrent (A)")
    ax.set_title("VEMD2704 photocurrent vs yield  (standard atmosphere)")
    ax.legend(ncol=3, loc="lower right", fontsize=8)


# --------------------------------------------------------------------------- #
# Figure 5 - VLOG vs yield, with ADC reference + VLOG ceiling
# --------------------------------------------------------------------------- #
def fig_vlog_vs_yield(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.2, 5.4)
    W = np.logspace(0, 3, 400)
    for d in DIST_KM:
        c = DIST_COLORS[d]
        ax.plot(W, vlog_from_current(photocurrent(W, d, "2nd")), color=c, lw=2.0,
                label=f"2nd pulse @ {d} km")
        ax.plot(W, vlog_from_current(photocurrent(W, d, "1st")), color=c, lw=1.5,
                ls="--", label=f"1st pulse @ {d} km")
    ax.axhline(ADC_VREF, color=C_LIMIT, ls=(0, (6, 3)), lw=1.4)
    ax.axhline(VLOG_CEILING, color="#ff8c8c", ls=(0, (3, 3)), lw=1.2)
    ax.text(1.1, ADC_VREF, " ADC max (2.5 V)", color=C_LIMIT, fontsize=8, va="bottom")
    ax.text(1.1, VLOG_CEILING, " VLOG ceiling (2.8 V)", color="#ff8c8c",
            fontsize=8, va="bottom")
    _yield_axis(ax)
    ax.set_ylim(0, 3.2)
    ax.set_ylabel("AD8304 VLOG (V)")
    ax.set_title("Log-amp output vs yield  (246 mV / decade)")
    ax.legend(ncol=3, loc="lower right", fontsize=8)


# --------------------------------------------------------------------------- #
# Figure 6 - yield estimate from measured t_min, with error band
# --------------------------------------------------------------------------- #
def fig_yield_from_tmin(fig):
    ax = fig.subplots()
    fig.set_size_inches(9.0, 5.2)
    tm = np.linspace(T_MIN_MIN_MS, T_MIN_MAX_MS, 400)
    W = W_from_min(tm)
    # A +-10% timing error -> ~+-18% yield (W ~ t^1.736). t_min is a coarse
    # cross-check: the minimum is a shallow, flat-bottomed feature, so its
    # measured time carries tens-of-percent uncertainty in practice.
    W_hi = W_from_min(tm * 1.10)
    W_lo = W_from_min(tm * 0.90)
    ax.fill_between(tm, W_lo, W_hi, color=C_ACCENT, alpha=0.18,
                    label="+-10% timing  (~+-18% yield)")
    ax.plot(tm, W, color=C_ACCENT, lw=2.4, label="yield estimate")
    for W0 in PRESETS_KT:
        ax.plot([t_min_ms(W0)], [W0], "o", color=C_ACCENT, ms=5, zorder=5)
    ax.set_xlim(T_MIN_MIN_MS, T_MIN_MAX_MS)
    ax.set_yscale("log")
    ax.set_ylim(5, 2000)
    ax.set_xlabel("Measured t_min (ms)")
    ax.set_ylabel("Estimated yield (kt)")
    ax.set_title("Yield from the peak->minimum time  W = (t_min / 1.87)^1.736")
    ax.legend(loc="upper left")


# --------------------------------------------------------------------------- #
# Figure 7 - simulated AD8304 VLOG waveforms (6-panel small multiples)
# --------------------------------------------------------------------------- #
def fig_vlog_waveforms(fig):
    fig.set_size_inches(11.0, 7.0)
    axes = fig.subplots(2, 3, sharex=False)
    REF_KM = 10  # reference range for the absolute current levels
    for ax, W, c in zip(axes.ravel(), PRESETS_KT, YIELD_COLORS):
        tm, ti = float(t_min_ms(W)), float(t_inter_ms(W))
        t = np.logspace(np.log10(0.3), np.log10(ti * 6), 1200)
        p = double_pulse_power(t, W)
        p /= p.max()
        # Map normalised power -> photocurrent: 2nd-pulse peak sits at the
        # report's I_2nd(W, 10 km); a baseline floor ~2 decades down.
        i_peak = float(photocurrent(W, REF_KM, "2nd"))
        i_floor = i_peak / 130.0
        i = i_floor + p * (i_peak - i_floor)
        v = vlog_from_current(i)
        ax.plot(t, v, color=c, lw=1.6)
        ax.fill_between(t, v.min(), v, color=c, alpha=0.10)
        ax.axvline(tm, color=C_LIMIT, ls=":", lw=1.1)
        ax.axvline(ti, color=C_TINTER, ls=":", lw=1.1)
        ax.set_xscale("log")
        ax.set_xlim(0.3, ti * 6)
        ax.set_ylim(0.6, 2.7)
        lab = f"{W} kt" if W < 1000 else "1 Mt"
        ax.set_title(f"{lab}   t_min={tm:.0f}  t_inter={ti:.0f} ms",
                     fontsize=10)
        ax.tick_params(labelsize=8)
    for ax in axes[-1, :]:
        ax.set_xlabel("Time (ms, log)", fontsize=9)
    for ax in axes[:, 0]:
        ax.set_ylabel("VLOG (V)", fontsize=9)
    fig.suptitle("Simulated AD8304 VLOG waveforms  (red = t_min, green = t_inter)",
                 fontsize=13, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.97))


# --------------------------------------------------------------------------- #
# Block diagrams - rendered replacements for the READMEs' ASCII signal chains
# --------------------------------------------------------------------------- #
DG_FE, DG_CMP, DG_ADC = "#f59e0b", "#3b82f6", "#10b981"   # front-end / comparator / ADC
DG_FW, DG_USB, DG_NET = "#a855f7", "#3b82f6", "#10b981"   # firmware / USB / network


def _diagram_axes(fig, w_in, h_in, xlim, ylim):
    """Borderless equal-aspect axes + themed box/arrow drawing helpers."""
    fig.set_size_inches(w_in, h_in)
    ax = fig.subplots()
    ax.set_xlim(*xlim)
    ax.set_ylim(*ylim)
    ax.set_aspect("equal")
    ax.axis("off")
    dark = to_hex(plt.rcParams["figure.facecolor"]) == to_hex(
        THEMES["dark"]["figure.facecolor"])
    C = {"face": "#1d2430" if dark else "#f3f5fa",
         "txt": "#e8ecf4" if dark else "#1b1f27",
         "line": "#7a8596" if dark else "#98a0ac"}

    def box(cx, cy, w, h, title, sub=None, accent="#888888", big=False,
            lines=None):
        ax.add_patch(FancyBboxPatch(
            (cx - w / 2, cy - h / 2), w, h,
            boxstyle="round,pad=0,rounding_size=0.16", linewidth=1.7,
            edgecolor=accent, facecolor=C["face"], zorder=3))
        if lines is not None:                       # title + stacked content lines
            ax.text(cx, cy + h / 2 - 0.34, title, ha="center", va="center",
                    color=accent, fontsize=11, fontweight="bold", zorder=4)
            y = cy + h / 2 - 0.82
            for ln in lines:
                ax.text(cx, y, ln, ha="center", va="center", color=C["txt"],
                        fontsize=8.6, zorder=4)
                y -= 0.46
        elif sub:                                   # title + one subtitle
            dyt, dys = (0.27, 0.31) if big else (0.21, 0.25)
            ax.text(cx, cy + dyt, title, ha="center", va="center",
                    color=accent, fontsize=12 if big else 10.5,
                    fontweight="bold", zorder=4)
            ax.text(cx, cy - dys, sub, ha="center", va="center",
                    color=C["txt"], fontsize=8.4, zorder=4)
        else:                                       # title only
            ax.text(cx, cy, title, ha="center", va="center", color=accent,
                    fontsize=12 if big else 10.5, fontweight="bold", zorder=4)

    def arrow(pts, color=None, ls="-"):
        color = color or C["line"]
        if len(pts) > 2:
            ax.plot([p[0] for p in pts], [p[1] for p in pts], color=color,
                    lw=1.7, ls=ls, zorder=2, solid_capstyle="round",
                    solid_joinstyle="round")
        ax.add_patch(FancyArrowPatch(
            pts[-2], pts[-1], arrowstyle="-|>", mutation_scale=15, lw=1.7,
            color=color, linestyle=ls, shrinkA=0, shrinkB=3, zorder=2))

    return ax, box, arrow, C


def fig_block_diagram(fig):
    """Top-level README: photons -> front-end -> firmware -> companion / GitHub."""
    ax, box, arrow, C = _diagram_axes(fig, 11.0, 6.9, (0, 16), (0, 10))
    box(2.1, 8.7, 2.0, 1.1, "VEMD2704", "photodiode", DG_FE)
    box(4.9, 8.7, 2.0, 1.1, "AD8304", "log amp", DG_FE)
    box(11.3, 8.7, 4.6, 1.1, "LT1711 comparator",
        "leading-edge timing · sub-µs", DG_CMP)
    box(9.3, 6.9, 4.6, 1.1, "AD7091R ADC", "SPI · waveform shape", DG_ADC)
    box(9.5, 5.3, 9.2, 1.4, "RP2350 · Pico 2 W firmware",
        "detection state machine · yield maths · BIST", DG_FW, big=True)
    box(5.0, 2.5, 5.4, 1.3, "Companion app",
        "USB serial · drive · watch · configure", DG_USB)
    box(12.5, 2.5, 5.4, 1.3, "GitHub JSON",
        "WiFi / HTTPS · one record per event", DG_NET)
    ax.plot([6.4], [8.7], "o", color=C["line"], ms=5, zorder=4)
    arrow([(0.45, 8.7), (1.1, 8.7)], DG_FE)
    ax.text(0.05, 9.08, "light", color=DG_FE, fontsize=9, ha="left",
            va="center", fontweight="bold")
    arrow([(3.1, 8.7), (3.9, 8.7)])
    arrow([(5.9, 8.7), (9.0, 8.7)])
    arrow([(6.4, 8.7), (6.4, 6.9), (7.0, 6.9)])
    arrow([(13.3, 8.15), (13.3, 6.0)])
    arrow([(9.3, 6.35), (9.3, 6.0)])
    arrow([(5.0, 4.6), (5.0, 3.15)])
    arrow([(12.5, 4.6), (12.5, 3.15)])
    ax.set_title("Signal chain — photons in, published yield out",
                 color=C["txt"], fontsize=13, fontweight="bold", pad=10)


def fig_code_dataflow(fig):
    """code/README.md: front-end -> firmware -> companion (USB) / GitHub (WiFi)."""
    ax, box, arrow, C = _diagram_axes(fig, 10.5, 6.0, (0, 16), (0, 9))
    box(3.4, 6.6, 4.2, 1.3, "Sensor + comparator",
        "photodiode · edge + shape", DG_FE)
    box(10.7, 6.6, 6.2, 1.5, "optical_bhang firmware",
        "RP2350 · Pico 2 W — detect · yield · BIST", DG_FW, big=True)
    box(4.3, 2.0, 5.8, 1.5, "bhang_companion",
        "USB serial · drive · watch · plot", DG_USB)
    box(11.9, 2.0, 6.2, 1.5, "GitHub-hosted JSON",
        "WiFi / HTTPS · one record per event", DG_NET)
    arrow([(0.5, 6.6), (1.3, 6.6)], DG_FE)
    ax.text(0.1, 7.05, "light", color=DG_FE, fontsize=9, ha="left",
            va="center", fontweight="bold")
    arrow([(5.5, 6.6), (7.6, 6.6)])
    arrow([(8.9, 5.85), (8.9, 4.7), (4.3, 4.7), (4.3, 2.75)])
    arrow([(12.5, 5.85), (12.5, 4.7), (11.9, 4.7), (11.9, 2.75)])
    ax.set_title("Software & data flow — on-device detection to published record",
                 color=C["txt"], fontsize=12.5, fontweight="bold", pad=10)


def fig_pcb_boards(fig):
    """PCB/README.md: the two stacked boards joined by the module connector."""
    ax, box, arrow, C = _diagram_axes(fig, 9.6, 6.6, (0, 16), (0, 11))
    box(8.0, 8.0, 13.0, 2.4, "BHG-3000 · sensor head   (20.1 × 12.5 mm)",
        accent=DG_FE,
        lines=["VEMD2704 photodiode  →  AD8304 log amp  →  VOUT",
               "+ white HSMW-C191 BIST LED for self-test"])
    box(8.0, 2.6, 13.0, 2.4, "bhangmeterv3 · carrier board   (80 × 40 mm)",
        accent=DG_CMP,
        lines=["LT1711 comparator → edge timing    ·    AD7091R SPI ADC → waveform",
               "Raspberry Pi Pico 2 W  ·  status LEDs  ·  USB"])
    arrow([(8.0, 10.5), (8.0, 9.2)], DG_FE)
    ax.text(8.0, 10.7, "incoming light", color=DG_FE, fontsize=9,
            ha="center", va="bottom", fontweight="bold")
    arrow([(8.0, 6.8), (8.0, 3.8)])
    for yy in (5.55, 5.05):
        ax.plot([7.3, 8.7], [yy, yy], color=C["line"], lw=2.2, zorder=4,
                solid_capstyle="round")
    ax.text(9.1, 5.3, "Mill-Max 800-series\nboard-to-board connector",
            color=C["txt"], fontsize=8.4, ha="left", va="center")
    ax.set_title("Two boards — optical sensor head over the carrier",
                 color=C["txt"], fontsize=12.5, fontweight="bold", pad=10)


def fig_sensorhead(fig):
    """PCB/BHG-3000/README.md: log-amp chain + BIST LED that lights the diode."""
    ax, box, arrow, C = _diagram_axes(fig, 11.0, 5.6, (0, 16), (0, 8))
    box(3.0, 6.0, 3.0, 1.1, "VEMD2704", "photodiode", DG_FE)
    box(7.2, 6.0, 3.0, 1.1, "AD8304", "log amp", DG_FE)
    box(12.3, 6.0, 4.2, 1.1, "VOUT → carrier", "(bhangmeterv3)", DG_CMP)
    box(3.0, 2.3, 3.4, 1.0, "HSMW-C191", "white BIST LED", DG_FE)
    box(7.5, 2.3, 2.8, 1.0, "BSS138", "LED driver", DG_ADC)
    arrow([(0.4, 6.0), (1.5, 6.0)], DG_FE)
    ax.text(0.1, 6.45, "light", color=DG_FE, fontsize=9, ha="left",
            va="center", fontweight="bold")
    arrow([(4.5, 6.0), (5.7, 6.0)])
    arrow([(8.7, 6.0), (10.2, 6.0)])
    arrow([(10.5, 2.3), (8.9, 2.3)])
    ax.text(10.7, 2.3, "BIST_EN\n(from carrier)", color=C["txt"], fontsize=8,
            ha="left", va="center")
    arrow([(6.1, 2.3), (4.7, 2.3)])
    arrow([(3.0, 2.8), (3.0, 5.45)], DG_FE, ls=(0, (4, 3)))
    ax.text(3.45, 4.05, "illuminates\nphotodiode", color=DG_FE, fontsize=8,
            ha="left", va="center")
    ax.set_title("Sensor head — log-amp front-end + BIST self-test",
                 color=C["txt"], fontsize=12.5, fontweight="bold", pad=10)


def fig_carrier(fig):
    """PCB/bhangmeterv3/README.md: socket -> buffers -> comparator/ADC -> Pico."""
    ax, box, arrow, C = _diagram_axes(fig, 11.0, 6.4, (0, 16), (0, 9.5))
    box(1.9, 7.6, 2.8, 1.1, "sensor head", "BHG-3000", DG_FE)
    box(5.3, 7.6, 2.6, 1.1, "U1 · HSN-1000", "module socket", DG_FE)
    box(8.3, 7.6, 2.4, 1.1, "buffers", "SN74LXC1T14", DG_CMP)
    box(13.0, 7.6, 4.8, 1.0, "LT1711 comparator", "leading-edge timing", DG_CMP)
    box(11.0, 5.6, 4.8, 1.0, "AD7091R ADC", "SPI · waveform shape", DG_ADC)
    box(9.0, 2.5, 11.0, 1.5, "Raspberry Pi Pico 2 W",
        "P2 / P3 sockets · firmware: detect · yield · BIST", DG_FW, big=True)
    arrow([(3.3, 7.6), (4.0, 7.6)])
    arrow([(6.6, 7.6), (7.1, 7.6)])
    arrow([(9.5, 7.6), (10.6, 7.6)])
    ax.plot([9.95], [7.6], "o", color=C["line"], ms=5, zorder=4)
    arrow([(9.95, 7.6), (9.95, 6.1)])
    arrow([(14.2, 7.1), (14.2, 3.25)])
    arrow([(11.0, 5.1), (11.0, 3.25)])
    ax.set_title("Carrier read-out chain — edge + shape into the Pico",
                 color=C["txt"], fontsize=12.5, fontweight="bold", pad=10)


# --------------------------------------------------------------------------- #
# Figure - literature VLOG waveforms at 10 km, all presets overlaid, with the
# comparator threshold and background level marked.
# --------------------------------------------------------------------------- #
VLOG_LIT_THRESHOLD = 1.03  # V  (carrier R18/R20: 3.3 V x 1k / (2.21k + 1k))
VLOG_LIT_BACKGROUND = 0.9  # V


def fig_vlog_waveforms_literature(fig):
    ax = fig.subplots()
    fig.set_size_inches(10.5, 5.2)
    REF_KM = 10
    t = np.logspace(np.log10(0.05), np.log10(2e4), 1500)
    for W, c in zip(PRESETS_KT, YIELD_COLORS):
        tm = float(t_min_ms(W))
        p = double_pulse_power(t, W)
        p_max = p.max()          # keep the raw peak: the marker below needs it
        p = p / p_max
        i_peak = float(photocurrent(W, REF_KM, "2nd"))
        i_floor = i_peak / 130.0
        i = i_floor + p * (i_peak - i_floor)
        v = vlog_from_current(i)
        ax.plot(t, v, color=c, lw=1.8, label=f"{W} kt")
        pm = double_pulse_power(np.array([tm]), W) / p_max
        i_m = i_floor + pm * (i_peak - i_floor)
        v_m = float(vlog_from_current(i_m)[0])
        ax.plot([tm], [v_m], marker="v", color=c, ms=7, zorder=5)

    ax.axhline(VLOG_LIT_THRESHOLD, color=C_LIMIT, lw=1.6, zorder=4)
    ax.text(2.0, VLOG_LIT_THRESHOLD, f" comparator threshold {VLOG_LIT_THRESHOLD:.2f} V",
            color=C_LIMIT, fontsize=10, ha="left", va="bottom", zorder=4)
    ax.axhline(VLOG_LIT_BACKGROUND, color="#9aa3b2", lw=1.3, ls="--", zorder=4)
    ax.text(0.06, VLOG_LIT_BACKGROUND, f" background {VLOG_LIT_BACKGROUND:.1f} V",
            color="#9aa3b2", fontsize=10, ha="left", va="bottom", zorder=4)

    ax.set_xscale("log")
    ax.set_xlim(0.05, 2e4)
    ax.set_ylim(0.7, 2.6)
    ax.set_xlabel("time after detonation (ms)")
    ax.set_ylabel("AD8304 VLOG (V)")
    ax.set_title("VLOG at 10 km — literature waveform, repo amplitude calibration\n"
                  f"inter-pulse minimum stays ≥1.3 V at every preset → COMP_OUT "
                  f"never recrosses {VLOG_LIT_THRESHOLD:.2f} V mid-event",
                  fontsize=12)
    ax.legend(ncol=3, loc="upper right", fontsize=9)


FIGURES = {
    "block-diagram": fig_block_diagram,
    "code-dataflow": fig_code_dataflow,
    "pcb-boards": fig_pcb_boards,
    "sensorhead": fig_sensorhead,
    "carrier": fig_carrier,
    "double-pulse-overlay": fig_double_pulse_overlay,
    "double-pulse-annotated": fig_double_pulse_annotated,
    "timing-vs-yield": fig_timing_vs_yield,
    "photocurrent-vs-yield": fig_photocurrent_vs_yield,
    "vlog-vs-yield": fig_vlog_vs_yield,
    "yield-from-tmin": fig_yield_from_tmin,
    "vlog-waveforms": fig_vlog_waveforms,
    "vlog-waveforms-literature": fig_vlog_waveforms_literature,
}


def _sanity_checks():
    print("Sanity checks (must match README table + firmware):")
    for W in (10, 100, 1000):
        print(f"  W={W:>4} kt:  t_inter={t_inter_ms(W):6.1f} ms   "
              f"t_min={t_min_ms(W):5.1f} ms")
    # Report photocurrent spot-checks.
    assert abs(photocurrent(100, 5, "2nd") - 6.40e-4) / 6.40e-4 < 0.01
    assert abs(photocurrent(100, 5, "1st") - 5.85e-4) / 5.85e-4 < 0.01
    assert abs(photocurrent(1000, 10, "2nd") - 2.14e-4) / 2.14e-4 < 0.02
    print("  photocurrent power-law reconstruction matches report table.\n")


def main():
    _sanity_checks()
    print(f"Writing figures to {OUT_DIR}")
    for name, draw in FIGURES.items():
        _render(name, draw)
    print(f"\nDone: {2 * len(FIGURES)} PNGs ({len(FIGURES)} figures x dark/light).")


if __name__ == "__main__":
    main()
