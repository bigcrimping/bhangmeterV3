# Optical Bhangmeter — Firmware

Firmware for an optical bhangmeter running on an RP2350 (Raspberry Pi
Pico 2 W). It watches a photodiode front-end for the characteristic
double flash of a nuclear detonation, estimates the device's yield from
the inter-flash timing, and publishes each event to a GitHub-hosted JSON file
over WiFi.

The two cores split the work:

- **Core 0** — detection and control: a comparator interrupt that timestamps the
  sub-µs flash onset (at the 1.03 V threshold over a ~0.9 V background) and
  triggers ADC waveform capture (~7 kHz); the peak → shallow-minimum → second-peak
  timing — and hence the yield — is then recovered from the captured waveform by
  reversal-hysteresis analysis, not from the comparator (the shallow one-decade
  minimum never recrosses the threshold mid-event). Also the BIST self-test
  emitter and the USB-serial command parser.
- **Core 1** — networking: WiFi association, NTP time sync, and an HTTPS upload
  to the GitHub Contents API, plus a periodic liveness beacon.

A host-side [companion app](../bhang_companion) talks to the device over
USB-serial to drive it, watch its state, plot captured waveforms, and set it up.

---

## Files

| File | Role |
|---|---|
| [CMakeLists.txt](CMakeLists.txt) | Build script — target, linked SDK libraries, and the critical compile defs (`PICO_MAX_SHARED_IRQ_HANDLERS=8`, stack sizes). |
| [pico_sdk_import.cmake](pico_sdk_import.cmake) | Standard Pico SDK bootstrap (finds the SDK via `PICO_SDK_PATH` / the VS Code extension). |
| [optical_bhang_runtime.c](optical_bhang_runtime.c) | The application: detection state machine, ADC capture, yield math, BIST, serial CLI, `main()`. Pin map is at the top of the file. |
| [bhang_net.c](bhang_net.c) / [bhang_net.h](bhang_net.h) | WiFi association, NTP sync, and the GitHub HTTPS upload. |
| [bhang_config.c](bhang_config.c) / [bhang_config.h](bhang_config.h) | Flash-persisted device config and the `SET`/`SHOW`/`SAVE`/`CLEAR` commands. |
| [led.c](led.c) / [led.h](led.h) | Status-LED behaviour (WiFi / BIST / detection). |
| [lwipopts.h](lwipopts.h) | lwIP stack configuration. **`MEM_ALIGNMENT 4` here is what makes WiFi work on the Pico 2 W** — do not omit. |
| [mbedtls_config.h](mbedtls_config.h) | mbedTLS configuration for the HTTPS (TLS) upload path. |

These are the minimum files needed to compile a working binary. The Pico SDK
itself is not included — it is a prerequisite you install (below).

This folder also ships a prebuilt binary, `optical_bhang_runtime.uf2`, so you
can flash a device without setting up the toolchain (see
[Flash the prebuilt firmware](#flash-the-prebuilt-firmware-quickest) below). It
contains no credentials — those are entered at runtime.

---

## Prerequisites

Installed by you, not part of this folder:

- **Raspberry Pi Pico SDK 2.2.0** and the Arm GNU toolchain **`14_2_Rel1`**
  (the versions pinned in `CMakeLists.txt`). Easiest via the **Raspberry Pi
  Pico** VS Code extension, which also sets `PICO_SDK_PATH`.
- **CMake ≥ 3.13** and **Ninja**.
- Board: **`pico2_w`** (Pico 2 W / RP2350 with CYW43 WiFi) — set in `CMakeLists.txt`.
- The sensor hardware: photodiode front-end + analog comparator on `COMP_OUT`,
  AD7091R ADC on SPI, a BIST LED, and status LEDs (see the pin map at the top of
  [optical_bhang_runtime.c](optical_bhang_runtime.c)).

---

## Flash the prebuilt firmware (quickest)

A prebuilt `optical_bhang_runtime.uf2` is included in this folder, so no build
toolchain is required. Hold BOOTSEL while plugging the Pico 2 W into USB so
it mounts as a `RP2350` drive, then drag `optical_bhang_runtime.uf2` onto it. The
board reboots and runs the firmware. (You can also flash it via SWD / `picotool`.)

The prebuilt binary carries no WiFi or GitHub secrets — configure it over serial
on first boot (below).

## Build from source

If you'd rather build it yourself — or after changing the code — set up the SDK
environment and run:

```sh
cmake -B build -G Ninja
cmake --build build
```

Output: `build/optical_bhang_runtime.uf2`, flashed the same way as above.

---

## First-boot configuration

There are **no credentials in the source** — WiFi and GitHub secrets are entered
at runtime over USB-serial and stored in the last flash sector. On a blank
device (first boot, or after `CLEAR`) the firmware enters config mode and refuses
to arm until you `SAVE`:

```
SET wifi_ssid  MyNetwork
SET wifi_pass  MyPassword
SET gh_user    myuser
SET gh_repo    myrepo
SET gh_token   github_pat_xxx      # fine-grained PAT, Contents: write
SET device_id  station-01
SET lat        51.5
SET lon        -0.1
SHOW                                # review (secrets masked)
SAVE                                # persist to flash + continue boot
```

Config persists across reboots; `CLEAR` factory-resets it. Commands are plain
text terminated by newline; the verb is case-insensitive. See the
[companion app](../bhang_companion) for a GUI front-end to this protocol.

---

## Security notes

- Confirm before publishing that there are still **no credentials anywhere in
  the source tree** — the WiFi password and GitHub token are typed in over
  serial and live only in flash. Never hard-code a token.
- **The TLS server certificate is not verified.** The HTTPS upload path creates
  its client config with no CA bundle, so the firmware does not authenticate
  `api.github.com`. The connection is encrypted but vulnerable to an active
  man-in-the-middle. Mitigate by using a **dedicated fine-grained PAT scoped to
  the single publishing repo** with *Contents: write* only, rotating it if
  exposure is suspected, and operating on a trusted network segment.
