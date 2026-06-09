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

A high-yield fireball radiates ~35–45 % of its energy as light, but not
smoothly. Three phases produce **two** distinct optical peaks:

1. **First pulse — radiative fireball.** The fireball surface cools through
   ~10⁴ K and briefly shines bright in the visible. Short (~1 ms) and nearly
   *independent of yield* — its timescale is set by air opacity, not energy.
2. **The minimum.** A hydrodynamic shock detaches and races ahead, ionising the
   air into an opaque shell that hides the (still hotter) interior. Visible
   emission drops 1–2 orders of magnitude.
3. **Second pulse — shock breakaway.** The shock cools below ~3000 K and turns
   transparent again, revealing a ~7500 K interior. Lower peak, but it lasts
   hundreds of ms to seconds and carries ~99 % of the integrated thermal yield.

**The gap between the two peaks scales with yield — and that gap is what the
instrument measures.** Standard low-altitude scaling: `t_inter ≈ 32·W^0.44 ms`
(`t_min ≈ t_inter/10`), inverted to two independent yield estimators whose
agreement is itself a sanity check.

| Yield | T_min (peak→min) | T_inter (peak→peak) |
|---|---:|---:|
| 10 kt | 9 ms | 88 ms |
| 50 kt | 18 ms | 179 ms |
| 100 kt | 24 ms | 243 ms |
| 250 kt | 36 ms | 363 ms |
| 500 kt | 49 ms | 493 ms |
| 1 Mt | 67 ms | 669 ms |

Design envelope: **10 kt – 1 Mt**. (High-altitude bursts lack the hydrodynamic
minimum and fall outside this instrument's scope.)

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
valid 10 kt – 1 Mt band. That combination rejects the plausible impostors —
lightning, camera flashes, welders, sun glints, meteors — none of which produce
peak → minimum → second-peak.

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
