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
- **Core 1** — the blocking network work: HTTPS uploads to the GitHub Contents
  API, the periodic liveness beacon, and WiFi reconnects. (The initial WiFi
  join and NTP sync run on core 0 at boot, before the detector is armed, because
  the WiFi chip's interrupt must live on the same core as the comparator's.)

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

> The prebuilt binary is rebuilt by hand and can lag the source. If it matters
> that you run exactly this code — the detection analysis in particular — build
> from source.

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

`gh_branch` (`main`), `gh_file` (`events_optical.json`) and `ntp`
(`pool.ntp.org`) have defaults and only need setting to change them. WiFi is
WPA2-PSK only, and the regulatory domain is fixed at build time (`WIFI_COUNTRY`,
default UK).

---

## Operating the device

### Serial console

The console is on USB CDC (any baud rate) and mirrored on UART0 — GP0 TX, GP1 RX,
1,000,000 baud. The device boots **armed** once WiFi bring-up finishes.

| Command | Effect |
|---|---|
| `ARM` / `DISARM` | Enable / disable the comparator trigger. `DISARM` also clears the capture buffer. |
| `STATUS` | Print state, BIST preset, and the last yield estimates. |
| `BIST <preset>` | Fire the self-test double flash and grade it. Presets: `10kt 50kt 100kt 250kt 500kt 1mt`. |
| `DUMP` | Stream the last captured waveform as CSV (`DUMP_START` … `DUMP_END`). |
| `SET <key> <value>` | Edit one config field in RAM. Only when `IDLE` (or in first-boot config mode). |
| `SHOW` | Print the config, secrets masked. |
| `SAVE` | Write the config to flash. Only when `IDLE`. |
| `CLEAR` | Erase the saved config; takes effect on the next boot. Only when `IDLE`. |
| `HELP` | List the commands. |

A real detection prints `YIELD tmin=<kt> kt tinter=<kt> kt`, with
` T_MIN_SUSPECT` appended when `t_min` falls outside its advisory window. A
trigger that fails the analysis prints `REJECT <reason>`. Either way the device
re-arms immediately.

### Button and LEDs

The push-button runs a BIST with the last-used preset (100 kt after boot).

| LED | Meaning |
|---|---|
| White | Blinks (with green) while joining WiFi; steady once connected. |
| Green | Steady during a BIST. Afterwards flashes five times for a pass. |
| Red | Steady: WiFi join failed, **or** a real detection (latched until reboot). Five flashes with green held on: BIST failed. One-second flash: an upload failed. |

### Published record

The device keeps one JSON object in `gh_file` and rewrites it in place:

```json
{"total minutes monitored": 30, "station": "station-01", "lat": 51.5, "long": -0.1,
 "last monitor upload date": "2026-01-01 12:00:00", "nuke gone off?": "no",
 "nuke detected time": null, "yield_from_minima_kt": null,
 "yield_from_interval_kt": null, "t_peak_to_min_ms": null,
 "t_peak_to_peak_ms": null, "total_captures": 0}
```

- **Beacon** — at boot and every 30 minutes, adds 30 to
  `total minutes monitored` and refreshes `last monitor upload date`.
- **Detection** — sets `nuke gone off?` to `"yes"`, `nuke detected time` to the
  flash onset (UTC), the two yields and timings, and increments
  `total_captures`. A failed upload is retried until it lands.
- **BIST results are never uploaded.**
- The firmware never sets `nuke gone off?` back to `"no"`. To clear an alarm,
  edit the file in the repo by hand (or delete it; the next beacon recreates it).
- If the record cannot be read for any reason other than "file not found", the
  device skips that write rather than overwrite it.

### Pin map

| GPIO | Signal | Notes |
|---:|---|---|
| 0 / 1 | UART0 TX / RX | Console mirror |
| 2, 6, 11 | White, green, red LED | Active high |
| 8 | `LATCH` | LT1711 latch enable; held low (transparent) |
| 9 | `COMP_OUT` | Comparator output; falls at flash onset |
| 12 | Push-button | Active low, internal pull-up. Schematic net `BIST_EN`. |
| 14 | BIST drive | Low = LED on (inverted by the carrier's buffer). Schematic net `BIST_OUT`; called `BIST_EN` in the firmware. |
| 16, 17, 18 | SPI0 MISO, CS, SCK | AD7091R |
| 20 | `ADC_CONVST` | AD7091R convert-start |

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
