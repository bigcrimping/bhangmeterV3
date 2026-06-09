# Bhangmeter Companion

Desktop **operator console** for the RP2350-based [optical bhangmeter](../optical_bhang).
It connects to the device over a **USB CDC serial** link (a virtual COM port)
and lets you:

- issue **control commands** — arm, disarm, run a self-test (BIST), request a dump;
- watch the device's **live state** — idle, armed, capturing, analysing, done;
- **plot the waveform** the device streams back after a capture;
- read and write the device's stored **configuration** (WiFi, location, GitHub
  upload target).

The device does all detection and yield maths on board; the Companion is a
window onto that work plus a remote control for it.

---

## Requirements

- **Python 3.11+**
- **PySide6** (Qt GUI + `QtSerialPort`), **pyqtgraph** (plotting), **numpy**
- **pyserial** — *optional*; only used for richer serial-port descriptions, with
  an automatic fallback to Qt's own port enumeration if it is absent.

## Install

```sh
python -m venv .venv
.venv\Scripts\activate            # Windows
# source .venv/bin/activate       # macOS/Linux

pip install -r requirements.txt   # pinned set
# or the minimal runtime deps:
# pip install PySide6 pyqtgraph numpy
```

(`requirements.txt` also lists dev tools — pytest/ruff/mypy — which are not
needed to run the app.)

## Run

This folder **is** the `bhang_companion` Python package, so launch it from the
**parent directory** with `-m`:

```sh
cd ..                 # into the directory that contains bhang_companion/
python -m bhang_companion
```

Running from *inside* this folder will fail — Python needs `bhang_companion` to
be importable as a top-level package, which it is from the parent.

On startup the app loads its dark theme, scans for serial ports, and
auto-connects if it finds something that looks like the device.

## Offline mode

**File → Open CSV** loads a previously saved DUMP file through the same parser
used for live captures — no device or serial connection needed.

---

## Serial / configuration protocol

All commands are plain-text lines terminated by `\n` (or `\r`); the verb is
case-insensitive and the device echoes each line before replying.

**Control:** `ARM`, `DISARM`, `STATUS`, `DUMP`, `HELP`, `BIST <preset>`
(presets: `10kt 50kt 100kt 250kt 500kt 1mt`).

**Configuration:**

| Command | Effect | Success reply |
|---|---|---|
| `SET <key> <value>` | Edit one field in the live RAM copy | `OK` |
| `SHOW` | Print every field (secrets masked) | `CONFIG:` + one line per field |
| `SAVE` | Persist to flash — **rejected unless `IDLE`** (DISARM first) | `OK SAVED` |
| `CLEAR` | Factory reset (takes effect next boot) | `OK CLEARED` |

Config keys: `wifi_ssid`, `wifi_pass`, `device_id`, `lat`, `lon`, `gh_user`,
`gh_repo`, `gh_branch`, `gh_file`, `gh_token`, `ntp`. `wifi_pass` and `gh_token`
are **secret** — `SHOW` masks them (first two + `...` + last two chars), so the
GUI keeps its own copy as the source of truth for save/restore.

`SET` edits only RAM; nothing is permanent until `SAVE`, and a flash write
cannot run during a capture (hence the `IDLE` requirement).

> **USB CDC ignores the baud rate** and runs at native USB speed. The baud
> dropdown in the UI is only for users on a UART-to-USB debug probe (e.g.
> Picoprobe).

---

## Layout

| Module | Role |
|---|---|
| [protocol.py](bhang_companion/protocol.py) | Pure functions + dataclasses (command formatters, line parser, `DumpAssembler`). No Qt — unit-testable on its own. |
| [serial_worker.py](bhang_companion/serial_worker.py) | `QObject` owning one `QSerialPort`; turns incoming bytes into one signal per line. |
| [main_window.py](bhang_companion/main_window.py) | Owns the worker + `DumpAssembler` and wires everything together. |
| [widgets/](bhang_companion/widgets/) | Read-only UI sinks driven by signals (console, control panel, status panel, waveform plot, config dialog). |
| [styles/coldwar.qss](bhang_companion/styles/coldwar.qss) | Dark "Cold War" theme (optional — the app runs without it). |
