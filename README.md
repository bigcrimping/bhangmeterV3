# Bhangmeter V3

An open-source **optical bhangmeter** — a fast photodetector that recognises the
unique two-peaked light curve of a nuclear detonation and estimates its **yield**
from the timing of that signature. This repository holds the complete build:
firmware, host software, PCB, and enclosure.

> A bhangmeter estimates yield from the optical waveform alone. No other natural
> or artificial event reproduces the double-pulse signature, which makes the
> waveform both the detection trigger and the measurement.

This is an **educational / amateur-science instrument** built from declassified,
publicly documented physics (Glasstone & Dolan and similar). It detects and
characterises an optical signature; it is not, and contains nothing of, a weapon.
In all likelihood it will spend its life running self-tests — that's the point.

---

## The double-pulse signature

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/double-pulse-overlay-dark.png">
    <img alt="Double-pulse optical signature, 10 kt to 1 Mt: a yield-independent first pulse near 1 ms and a second pulse that slides later as yield increases" src="assets/double-pulse-overlay-light.png" width="90%">
  </picture>
</p>

A high-yield fireball radiates ~35–45 % of its energy as light, but not
smoothly. Three phases produce **two** distinct optical peaks:

1. **First pulse — radiative fireball.** The fireball surface cools through
   ~10⁴ K and briefly shines bright in the visible — a ~10,000 K source, peaking
   in the UV (~290 nm) with a strong visible tail. Short (~1 ms) and nearly
   *independent of yield* — its timescale is set by air opacity, not energy.
2. **The minimum.** A hydrodynamic shock detaches and races ahead, ionising the
   air into an opaque shell that hides the (still hotter) interior. Visible
   emission drops 1–2 orders of magnitude.
3. **Second pulse — shock breakaway.** The shock cools below ~3000 K and turns
   transparent again, revealing a ~7500 K interior — near solar temperature, its
   blackbody peak ~380 nm with energy spread across the whole visible band. Lower
   peak, but it lasts hundreds of ms to seconds and carries ~99 % of the
   integrated thermal yield.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/double-pulse-annotated-dark.png">
    <img alt="Anatomy of a 100 kt double pulse: first peak near 1 ms, inter-pulse minimum at t_min 24 ms, second peak at t_inter 243 ms, with the three physical phases shaded" src="assets/double-pulse-annotated-light.png" width="80%">
  </picture>
</p>

**The gap between the two peaks scales with yield — and that gap is what the
instrument measures.** The firmware uses standard low-altitude Glasstone & Dolan
scaling[^gd] — `t_inter ≈ 32·W^0.44 ms` and `t_min ≈ 3.2·W^0.44 ms` (≈ `t_inter/10`)
— inverted to two independent yield estimators whose agreement is itself a sanity
check:

```
W = (t_inter / 32)^2.273        W = (t_min / 3.2)^2.273
```

| Yield | T_min (peak→min) | T_inter (peak→peak) |
|---|---:|---:|
| 10 kt | 9 ms | 88 ms |
| 50 kt | 18 ms | 179 ms |
| 100 kt | 24 ms | 243 ms |
| 250 kt | 36 ms | 363 ms |
| 500 kt | 49 ms | 493 ms |
| 1 Mt | 67 ms | 669 ms |

The device reports a yield only when the timing lands in the valid window —
`t_inter` 70–800 ms, `t_min` 7–80 ms — i.e. the **10 kt – 1 Mt** design envelope with
20 % slack. (High-altitude bursts lack the hydrodynamic minimum and fall outside this
instrument's scope.)

<p align="center">
  <picture><source media="(prefers-color-scheme: dark)" srcset="assets/timing-vs-yield-dark.png"><img alt="Pulse timing versus yield: t_min, t_inter and their difference on log-log axes" src="assets/timing-vs-yield-light.png" width="49%"></picture>
  <picture><source media="(prefers-color-scheme: dark)" srcset="assets/yield-from-tmin-dark.png"><img alt="Yield estimated from the measured peak-to-minimum time, with a plus or minus 25 percent band from 10 percent timing error" src="assets/yield-from-tmin-light.png" width="49%"></picture>
</p>

---

## How the instrument works

```
   light ─▶ VEMD2704 photodiode ─▶ AD8304 log amp ─┬─▶ LT1711 comparator ─▶ edge timing (sub-µs)
                                                   │
                                                   ├─▶ AD7091R ADC (SPI) ─▶ waveform shape
                                                   │
                                                   ▼
                                      RP2350 (Pico 2 W) firmware
                             detection state machine · yield maths · BIST
                                                   │
                 ┌─────────────────────────────────┴───────────────┐
                 ▼                                                 ▼
     USB serial → companion app                         WiFi/HTTPS → GitHub JSON
     (drive, watch, plot, configure)                  (one record per event/beacon)
```

A detection requires **all** of: a fast leading edge (comparator), a first peak
followed by a measurable minimum, and a second peak whose timing falls in the
valid 10 kt – 1 Mt band. That combination rejects the plausible impostors — none
of which reproduce the **peak → minimum → second-peak** sequence:

- **Lightning** — a single peak, typically &lt;50 ms, no slow second hump.
- **Camera flash, welder strike, arc fault** — one fast pulse, no second peak.
- **Sun glint, aircraft strobe** — slow rise, no millisecond-scale leading edge.
- **Meteor / bolide** — one broad envelope, not peak → minimum → peak.

---

## Optical front-end & dynamic range

The photodiode sees a huge range of light: at 5 km a 1 Mt first pulse drives
milliamps of photocurrent, while a far, hazy 10 kt second pulse sits down in the
nanoamps — about **seven decades**. Rather than switch gain mid-event, the **AD8304
logarithmic transimpedance amplifier** compresses that whole span onto one analog
output:

```
VLOG = 246 mV × log₁₀(I_pd / 1 pA)      ⇔      I_pd = 10^(VLOG / 0.246 − 12)
```

`RLOG = 123 kΩ` sets the 246 mV/decade slope, and a fixed **OD 2.0** neutral-density
filter (1 % transmission, effective aperture `A_eff = 1.51 × 10⁻⁸ m²`) pulls the
bright extreme back under the AD8304's 10 mA ceiling. First-pulse saturation is
*accepted*: the comparator has already timestamped the leading edge to sub-µs, and the
log output stays valid through the dimmer, slower second pulse that carries the timing.

<p align="center">
  <picture><source media="(prefers-color-scheme: dark)" srcset="assets/photocurrent-vs-yield-dark.png"><img alt="VEMD2704 photocurrent versus yield at 5, 10 and 20 km, staying between the AD8304 100 pA floor and 10 mA ceiling" src="assets/photocurrent-vs-yield-light.png" width="49%"></picture>
  <picture><source media="(prefers-color-scheme: dark)" srcset="assets/vlog-vs-yield-dark.png"><img alt="AD8304 VLOG output versus yield at 5, 10 and 20 km, staying under the 2.5 V ADC reference" src="assets/vlog-vs-yield-light.png" width="49%"></picture>
</p>

Across 10 kt – 1 Mt and 5–20 km the photocurrent stays inside the AD8304's
100 pA – 10 mA window and VLOG stays under the external ADC's 2.5 V reference — one
fixed-gain front-end, no switching. Bandwidth is modest: the first pulse rises in
~100 µs and is a few ms wide, the second pulse is sub-kHz, and the comparator (not the
ADC) carries the fast edge. The whole double pulse lands in one clean trace:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/vlog-waveforms-dark.png">
    <img alt="Simulated AD8304 VLOG waveforms for 10 kt to 1 Mt, each marked with its t_min and t_inter" src="assets/vlog-waveforms-light.png" width="92%">
  </picture>
</p>

---

## Repository layout

| Folder | Contents |
|---|---|
| [code/](code) | All software. The [firmware](code/optical_bhang) (RP2350 / Pico 2 W) and the [companion app](code/bhang_companion) (PySide6 desktop operator console). See [code/README.md](code/README.md). |
| [PCB/](PCB) | Hardware design — two boards: the **carrier** ([bhangmeterv3](PCB/bhangmeterv3)) and the **BHG-3000 sensor head** ([BHG-3000](PCB/BHG-3000)). Schematics, assembly drawings + BOMs, and Gerbers. See [PCB/README.md](PCB/README.md). |
| [mech/](mech) | Mechanical / enclosure design — 3D-printable models (e.g. STL/STEP) for the housing and optical mount. |

---

## Hardware at a glance

| Block | Part | Why |
|---|---|---|
| Photodiode | **VEMD2704** (Si PIN, ~400–1100 nm) | Both flashes are **broadband** thermal emission — a near-blackbody continuum across UV, visible, and IR, with the energy-dominant second pulse resembling sunlight (~6000–7000 K) (Glasstone & Dolan §7.23, §7.76). Left deliberately **unfiltered** so the detector gathers as much of that broadband light as possible. |
| Logarithmic amp | **AD8304** | Log-compresses the photodiode current so one fixed-gain front-end spans the dim baseline up to a saturating first pulse (several decades) — no gain switching mid-event. |
| Edge timing | **LT1711** comparator | Sub-µs timestamp of the leading edge regardless of amplitude (first-pulse saturation is accepted). |
| Waveform capture | **AD7091R** ADC (SPI) | Records the analog shape so peak/minimum/second-peak can be analysed. |
| Compute / comms | **RP2350** (Pico 2 W) | Dual-core: detection + control on core 0, WiFi/NTP/HTTPS upload on core 1. |
| Blast cross-check | Honeywell HSC pressure sensor | *(planned)* overpressure reference to back up the optical yield estimate. |

> **VEMD2704** — 1.51 mm² active area, ~0.40 A/W broadband responsivity, 17.6 pF,
> 70 ns rise, ±67° FOV. The AD8304 runs from the Pico's 3V3 rail (within its
> 3.0–5.5 V spec), behind a ferrite bead that isolates it from digital switching noise.

---

## Getting started

1. **Build the hardware** from [PCB/](PCB) and [mech/](mech).
2. **Flash the firmware** — a prebuilt `.uf2` is included, or build from source.
   See [code/optical_bhang/README.md](code/optical_bhang/README.md).
3. **Configure on first boot** over USB serial (WiFi + a GitHub repo to publish
   to). No credentials live in the firmware — they're entered at runtime and
   stored in flash.
4. **Drive and view** the device with the [companion app](code/bhang_companion),
   or read the published JSON from your GitHub repo.

---

## Status & scope

Work in progress. Yield estimation from timing is intrinsically coarse — a 10 %
timing error becomes a ~25 % yield error (`W ∝ T^2.27`), which is why two
estimators are used and a blast cross-reference is planned. The firmware's TLS
upload path does not yet verify the server certificate; use a dedicated,
fine-grained GitHub token (see the firmware README's security notes).

## References

- Glasstone, S. & Dolan, P. J. *The Effects of Nuclear Weapons*, 3rd ed., 1977 (§7.78–7.88).
- Bridgman, C. J. *Introduction to the Physics of Nuclear Weapons Effects*, DTRA, 2001.
- Brode, H. L. "Fireball Phenomenology," RAND, 1964.
- U.S. AEC, *Capabilities of Nuclear Weapons* (DNA EM-1), declassified excerpts.

[^gd]: The firmware's 32 ms coefficient sits on the low end of the literature.
    Glasstone & Dolan's canonical fit is `t_max ≈ 0.0417·W^0.44 s` (≈ 42 ms·W^0.44),
    ~30 % later; the exponent (0.44) is the canonical value. See
    [G&D §7.85](https://www.atomicarchive.com/resources/documents/effects/glasstone-dolan/chapter7.html).
