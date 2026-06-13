# Figure sources

The PNGs in [`assets/`](..) embedded by the top-level [README](../../README.md)
are generated, not hand-drawn. Each figure is written twice —
`<name>-dark.png` and `<name>-light.png` — so the README can serve a
theme-matched image through a `<picture>` + `prefers-color-scheme` block.

## Regenerate

```sh
python -m pip install -r assets/src/requirements.txt   # matplotlib + numpy
python assets/src/generate_figures.py                  # writes 24 PNGs to assets/
```

The script is self-contained (no network, no input files). It prints sanity
checks on start-up and overwrites the PNGs in place.

## Where the numbers come from

- **Timing / yield figures** use the firmware scaling constants verbatim —
  `t_inter = 41.7·W^0.44 ms`, `t_min = 1.87·W^0.576 ms` (different exponents,
  so the ratio is not a fixed 1/10), inverted as `W = (t_inter/41.7)^2.273` and
  `W = (t_min/1.87)^1.736` — so they agree with what the device computes
  (`code/optical_bhang/optical_bhang_runtime.c`).
- **The double-pulse waveform** is no longer an illustrative log-normal; it is
  **literature-anchored**, fitted to published power-and-energy and
  minimum-depth/timing data: Glasstone & Dolan 1977 §7.85 (`t_2max`, power),
  §7.86 (energy fraction), §7.03 (first-pulse energy), §2.125 (20 kt minimum),
  and the Nuclear Weapon Archive FAQ §5 (ten-fold minimum depth, second-peak
  recovery, 20 kt/1 Mt timings). The inter-pulse minimum sits at ~0.1 of the
  second-peak power (a ~1-decade dip, never dark).
- **Photocurrent / VLOG figures** reproduce the AD8304 + VEMD2704 design
  report. Its photocurrent-vs-yield curves are clean power laws
  (1st pulse `∝ W^0.70`, 2nd pulse `∝ W^0.56`, the latter following
  Glasstone & Dolan's `P_max ∝ W^0.56`), reconstructed from a few calibrated
  coefficients rather than copied arrays. Spot-checks match the report table to
  the quoted precision.

To restyle (sizes, palette, themes) or add a figure, edit the corresponding
`fig_*` function and the `FIGURES` / `THEMES` tables in
[`generate_figures.py`](generate_figures.py).
