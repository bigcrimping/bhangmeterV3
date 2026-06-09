# Figure sources

The PNGs in [`assets/`](..) embedded by the top-level [README](../../README.md)
are generated, not hand-drawn. Each figure is written twice —
`<name>-dark.png` and `<name>-light.png` — so the README can serve a
theme-matched image through a `<picture>` + `prefers-color-scheme` block.

## Regenerate

```sh
python -m pip install -r assets/src/requirements.txt   # matplotlib + numpy
python assets/src/generate_figures.py                  # writes 14 PNGs to assets/
```

The script is self-contained (no network, no input files). It prints sanity
checks on start-up and overwrites the PNGs in place.

## Where the numbers come from

- **Timing / yield / waveform figures** use the firmware scaling constants
  verbatim — `t_inter = 32·W^0.44 ms`, `t_min = 3.2·W^0.44 ms`,
  `W = (t/k)^2.273` — so they agree with what the device computes
  (`code/optical_bhang/optical_bhang_runtime.c`).
- **Photocurrent / VLOG figures** reproduce the AD8304 + VEMD2704 design
  report. Its photocurrent-vs-yield curves are clean power laws
  (1st pulse `∝ W^0.70`, 2nd pulse `∝ W^0.56`, the latter following
  Glasstone & Dolan's `P_max ∝ W^0.56`), reconstructed from a few calibrated
  coefficients rather than copied arrays. Spot-checks match the report table to
  the quoted precision.

To restyle (sizes, palette, themes) or add a figure, edit the corresponding
`fig_*` function and the `FIGURES` / `THEMES` tables in
[`generate_figures.py`](generate_figures.py).
