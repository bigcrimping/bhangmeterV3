# Changes since the initial commit

Everything below is uncommitted on `main` on top of `1f2b5a3` (Initial commit).
It came out of a review of the firmware, the companion app, the READMEs and the
PCB documentation.

**Verification status**

- Firmware: builds cleanly with Pico SDK 2.2.0 / toolchain `14_2_Rel1`
  (no warnings or errors from the project sources). **Not flashed or run on
  hardware.**
- Companion: the updated line parser was exercised against sample firmware
  output and parsed as intended. **The GUI was not launched.**
- Docs: every relative link and image path in the repo's markdown resolves.

---

## Firmware (`code/optical_bhang`)

### Bug fixes

| Problem | Fix | Where |
|---|---|---|
| A transient failure reading the status file (timeout, 5xx, rate limit) was treated as "file missing", so the device wrote a fresh default record — resetting `total minutes monitored` and setting `nuke gone off?` back to `"no"`. | The fetch now returns found / absent / error. Only "absent" (HTTP 404, or a complete 200 with no JSON object) creates a new record. On an error the beacon is skipped and a detection is held for retry. | `bhang_net.c` |
| `nuke detected time` was the time of the upload, not of the detection, and was `1970-01-01` without NTP. A retried upload could be hours late. | The flash-onset timestamp now travels with the queued event and is converted to UTC at upload time. If the clock has never synced the field is `null`. | `bhang_net.c`, `bhang_net.h`, `optical_bhang_runtime.c` |
| A BIST did not clear the previous event's results before analysing, so a failed run could show old yields against the new waveform in `STATUS` and `DUMP`. | `do_bist` resets the timing and yield values first, as the real-detection path already did. | `optical_bhang_runtime.c` |
| A second real detection was dropped if one was already waiting for an upload retry. | Nothing is dequeued while an upload is held, so later detections wait in the queue (depth 4) and upload in order. | `optical_bhang_runtime.c` |
| `total_captures` counted BIST passes and reset to zero on reboot, but was uploaded as a lifetime count. | The count is now read from the published record and incremented once per uploaded detection. The in-RAM counter is removed. | `bhang_net.c`, `optical_bhang_runtime.c` |

### Smaller changes

- `CLEAR` is now refused unless the device is `IDLE` (or in first-boot config
  mode), matching `SAVE`. `HELP` text updated.
- `ARM` and `BIST` are refused during first-boot config mode with
  `ERR config_mode (SAVE first)`. In that mode the comparator interrupt handler
  is not yet registered, so enabling the interrupt was unsafe.
- Removed the `printf` from the NTP callback, which runs in interrupt context.
- The status fetch looks for the JSON object in the response body only, not in
  the headers.

### API change

`net_upload_optical()` takes the flash-onset time and no longer takes a capture
count:

```c
bool net_upload_optical(const char *source, uint64_t t0_boot_us,
                        float t_min_ms, float t_inter_ms,
                        float yield_tmin_kt, float yield_tinter_kt);
```

### Comments corrected

- `bhang_net.c` header no longer says "single-core"; removed a reference to an
  "edge-band fallback path" that no longer exists.
- `bhang_net.h` describes the current upload behaviour (BIST results are never
  uploaded).
- `led.c` / `led.h` / `optical_bhang_runtime.c`: the red detection latch lasts
  until reboot (a BIST pass does not clear it). `led.h` no longer says "V2" or
  cites a missing `bhangmeter_led_behaviour.md`.
- Comparator threshold in comments changed from 1.1 V to 1.03 V, the value the
  carrier's `R18`/`R20` divider gives. The `P1_FLOOR_COUNTS` constant itself
  (1100 mV) is unchanged.

### Prebuilt binary

`optical_bhang_runtime.uf2` is rebuilt from the current source. The previous
binary predated it: it contained the old BIST result line and an older
detection path.

---

## Companion app (`code/bhang_companion`)

| Problem | Fix | Where |
|---|---|---|
| Config push sent the `SET`s before `DISARM`. The firmware rejects `SET` unless `IDLE` and boots armed, so every `SET` failed and the old config was saved with "OK SAVED". | Push now sends `DISARM` → `SET`s → `SAVE`, then `ARM` if the device was armed beforehand. | `main_window.py` |
| A real detection's `YIELD tmin=… tinter=… [T_MIN_SUSPECT]` line was not parsed, so the GUI showed nothing until a manual `STATUS` or `DUMP`. | New `YieldReport` message; the status panel updates and the console and status bar note the detection and any `T_MIN_SUSPECT` flag. | `protocol.py`, `main_window.py` |
| `ERR …` replies were ignored. | New `DeviceError` message, shown in the console and status bar. | `protocol.py`, `main_window.py` |

---

## Documentation

### `README.md`

- Replaced a hidden TODO with the derivation of the log-amp transfer
  (246 mV/decade, ≈1 pA intercept) from the BHG-3000 resistor values and AD8304
  datasheet nominals.
- Defined the `[^gd]` footnote, which previously rendered as literal text.
- References: added the Nuclear Weapon Archive FAQ, which the text cites; widened
  the Glasstone & Dolan section range to cover the sections actually cited.
- Comparator threshold is 1.03 V throughout (one passage and one alt text said
  1.05 V).
- `t_min` is described as the firmware treats it: advisory, flagging
  `T_MIN_SUSPECT`, not a condition for reporting a yield.
- 100 kt `t_min` corrected from 26 ms to 27 ms (26.5 ms rounds up).
- `mech/` described as the `.3mf` files it contains.
- Core split and companion caption reworded to match what the code does.

### `code/optical_bhang/README.md`

- New "Operating the device" section: serial console and full command table,
  button and LED meanings, the published JSON record (fields, beacon, detection,
  how to clear an alarm), and a GPIO pin map.
- Core description corrected: the boot WiFi join and NTP sync run on core 0.
- Notes on config defaults, WPA2-only WiFi and the build-time country code.
- Note that the prebuilt binary can lag the source.

### `code/bhang_companion/README.md`

- Fixed the screenshot path and all five links in the Layout table.
- Python requirement stated as 3.11 or 3.12 for the pinned `requirements.txt`.
- Notes on Windows-only auto-connect, BIST buttons needing `DISARM`, re-arm
  after a config push, and secrets appearing in clear text in the console, saved
  logs and saved config files.
- `CLEAR` shown as `IDLE`-only.

### `PCB/bhangmeterv3/README.md`

- Bill of materials corrected to match `Assembly.pdf`: seven parts marked
  *NO FIT* were listed as fitted (`C6`, `C16`, `R1`, `R4`, `R12`, `R15`, `R19`),
  and `R18` was listed as 10 kΩ when it is 2.21 kΩ.
- Added the comparator threshold and its divider, and a module-socket pin table
  with a note on the HSN-1000 naming.

### `PCB/BHG-3000/README.md`

- Added the connector pinout. 3V3 does not cross the connector, contrary to the
  previous text.
- `P1` is two 7-pin strips.
- `R1` (309 kΩ) is the *Do Not Fit* part and `R2` (24.9 kΩ) is fitted; the
  README had them the other way round.
- Added a warning to leave solder bridge `SB1` open.
- Noted that the 1 µF capacitors are 0603.

### Figures (`assets/`)

- `assets/src/generate_figures.py`: comparator threshold set to 1.03 V; fixed the
  inter-pulse minimum marker in the literature VLOG figure, which was normalised
  by the wrong value; replaced the slope TODO with the derivation.
- Regenerated `vlog-waveforms-literature-dark.png` and `-light.png` only.
- `assets/src/README.md`: the script writes 26 PNGs, not 24.

---

## Known issues not addressed

**Detection behaviour (design decisions, not bugs)**

- Acceptance needs only a peak → 60 mV dip → peak with `t_inter` in
  90–1050 ms. Dip depth, first-pulse width and agreement between the two yield
  estimates are not checked, so other double flashes could pass.
- Arming does not check whether the comparator is already tripped. If ambient
  light is above the fixed threshold the device reports `ARMED` but cannot
  trigger.
- At 1 Mt the second peak may not be confirmed before the 1.75 s capture buffer
  ends.
- The detector is unarmed during boot networking, which can take about two
  minutes if the access point is down.
- TLS does not verify the server certificate (already documented in the
  firmware README).

**Companion**

- The pinned dependencies do not install on Python 3.13 or later.
- Secrets appear in the console and in saved files (now documented, not changed).
- BIST buttons are disabled while armed; auto-connect only recognises the
  Windows port description; a truncated dump is accepted without checking the
  sample count; there are no tests.

**Documentation and repository**

- No licence file.
- No `mech/` README, no part number or mounting detail for the OD 2.0 filter,
  and no Altium source files.
- The "recovers yield to ±15 % in simulation" claim has no simulation in the
  repository.
- Unused tracked assets: `assets/test.png`, `assets/module.jpg`, and the four
  `*-literature` PNGs other than `vlog-waveforms-literature`.
- The second-pulse temperature is given as ~7500 K in one place and
  ~6000–7000 K in another.
- In the waveform model behind the figures, the curve's minimum falls later
  than the `t_min` formula at low yields (about 14 ms against 7 ms at 10 kt).
- `R1`/`R2` fitting on the BHG-3000 was inferred from the schematic and the
  matching arithmetic; confirm against a built board.
