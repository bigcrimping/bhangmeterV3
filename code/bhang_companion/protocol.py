"""Pure protocol functions — no Qt imports. Safe to import without a display."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional
import numpy as np

PRESETS = ("10kt", "50kt", "100kt", "250kt", "500kt", "1mt")

CONFIG_KEYS = (
    "wifi_ssid", "wifi_pass", "device_id", "lat", "lon",
    "gh_user", "gh_repo", "gh_branch", "gh_file", "gh_token", "ntp",
)
SECRET_KEYS = frozenset({"wifi_pass", "gh_token"})

CONFIG_MODE_BANNER = "=== CONFIG MODE: no valid config in flash ==="

# ---------------------------------------------------------------------------
# Command formatters
# ---------------------------------------------------------------------------

def cmd_arm() -> str:
    return "ARM\n"

def cmd_disarm() -> str:
    return "DISARM\n"

def cmd_status() -> str:
    return "STATUS\n"

def cmd_dump() -> str:
    return "DUMP\n"

def cmd_help() -> str:
    return "HELP\n"

def cmd_bist(preset: str) -> str:
    if preset not in PRESETS:
        raise ValueError(f"Unknown BIST preset {preset!r}; valid: {PRESETS}")
    return f"BIST {preset}\n"

def cmd_set(key: str, value: str) -> str:
    if key not in CONFIG_KEYS:
        raise ValueError(f"Unknown config key {key!r}; valid: {CONFIG_KEYS}")
    return f"SET {key} {value}\n"

def cmd_show() -> str:  return "SHOW\n"
def cmd_save() -> str:  return "SAVE\n"
def cmd_clear() -> str: return "CLEAR\n"

def parse_show_line(line: str) -> "tuple[str, str] | None":
    """'  wifi_ssid  = MyNetwork' -> ('wifi_ssid', 'MyNetwork'); else None."""
    if "=" not in line:
        return None
    key, _, val = line.partition("=")
    key = key.strip()
    return (key, val.strip()) if key in CONFIG_KEYS else None

def mask_secret(value: str) -> str:
    """Return the masked form of a secret as printed by SHOW."""
    n = len(value)
    if n == 0:
        return "(empty)"
    if n <= 4:
        return "****"
    return value[:2] + "..." + value[-2:]

def verify_show_field(key: str, stored: str, shown: str) -> bool:
    """Return True when a SHOW value is consistent with the stored value.

    Non-secret fields must match exactly. Secret fields are checked against the
    mask rule since the clear-text value never appears in SHOW output.
    """
    if key not in SECRET_KEYS:
        return stored == shown
    return shown == mask_secret(stored)

# ---------------------------------------------------------------------------
# Parsed message dataclasses
# ---------------------------------------------------------------------------

@dataclass
class StateChange:
    name: str  # IDLE / ARMED / CAPTURING / ANALYSING / DONE / REJECT

@dataclass
class StatusReport:
    """Parsed from the STATUS reply: 'state=… preset=… yield_tmin=… yield_tinter=…'."""
    state: str
    preset: Optional[str] = None
    yield_tmin_kt: Optional[float] = None    # None when firmware emits 'N/A'
    yield_tinter_kt: Optional[float] = None

@dataclass
class BistDone:
    preset: str
    passed: bool = True
    yield_tinter_kt: Optional[float] = None   # tinter yield (both line formats)
    yield_tmin_kt: Optional[float] = None      # legacy form only: tmin as a kt yield
    tmin_ms: Optional[float] = None            # new GEOM form: geometric tmin in ms
    tmin_expect_ms: Optional[float] = None     # new GEOM form: expected geometric tmin (ms)
    reject_reason: Optional[str] = None        # populated on a failing BIST

@dataclass
class Reject:
    reason: str

@dataclass
class YieldReport:
    """A real detection: 'YIELD tmin=… kt tinter=… kt [T_MIN_SUSPECT]'."""
    yield_tmin_kt: Optional[float]
    yield_tinter_kt: Optional[float]
    tmin_suspect: bool = False

@dataclass
class DeviceError:
    """An 'ERR …' reply to a command."""
    message: str

@dataclass
class DumpHeader:
    t0_epoch_us: int
    sample_rate_hz: int
    samples: int
    yield_tmin_kt: Optional[float]    # None when firmware emits 'N/A'
    yield_tinter_kt: Optional[float]
    t_peak_to_min_ms: Optional[float] = None   # raw peak→min inter-pulse timing
    t_peak_to_peak_ms: Optional[float] = None  # raw peak→peak inter-pulse timing

@dataclass
class DumpRow:
    t_us: int
    adc_mv: int

@dataclass
class EdgeEvent:
    t_us: int
    direction: str  # "ONSET" (flash onset) or "END" (flash end), per firmware DUMP

@dataclass
class Capture:
    header: DumpHeader
    t_us: np.ndarray   # int64, length = header.samples
    adc_mv: np.ndarray # int16
    edges: list  # list[EdgeEvent], may be empty

# ---------------------------------------------------------------------------
# Line parser — discrete messages only (not DUMP stream)
# ---------------------------------------------------------------------------

_VALID_STATES = {"IDLE", "ARMED", "CAPTURING", "ANALYSING", "DONE", "REJECT"}


def _parse_float_or_none(s: str) -> Optional[float]:
    s = s.strip()
    if s.upper() in ("N/A", "NULL"):
        return None
    return float(s)


def _kv_tokens(stripped: str) -> dict:
    """Split a whitespace-delimited line into a {key: value} map for the
    'key=value' tokens, lower-casing keys. Bare tokens (e.g. the 'kt' unit
    suffixes the firmware prints) are ignored."""
    out: dict = {}
    for tok in stripped.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k.lower()] = v
    return out


def parse_line(
    line: str,
) -> "StateChange | StatusReport | BistDone | Reject | YieldReport | DeviceError | None":
    """Parse a single stripped line. Returns None if unrecognised or a DUMP stream line."""
    stripped = line.strip()
    if not stripped:
        return None

    upper = stripped.upper()

    # STATUS reply: "state=IDLE preset=100kt yield_tmin=98.2 kt yield_tinter=101.5 kt"
    if "STATE=" in upper:
        kv = _kv_tokens(stripped)
        name = kv.get("state", "").upper()
        if name in _VALID_STATES:
            return StatusReport(
                state=name,
                preset=kv.get("preset"),
                yield_tmin_kt=_parse_float_or_none(kv.get("yield_tmin", "N/A")),
                yield_tinter_kt=_parse_float_or_none(kv.get("yield_tinter", "N/A")),
            )

    # State transition: "STATE ARMED" (firmware), or "STATE: ARMED" / "STATUS IDLE"
    for prefix in ("STATE:", "STATE ", "STATUS:", "STATUS "):
        if upper.startswith(prefix):
            rest = stripped[len(prefix):].strip().upper()
            if rest in _VALID_STATES:
                return StateChange(name=rest)
            break

    # Bare state name on its own line
    if upper in _VALID_STATES:
        return StateChange(name=upper)

    # BIST result. Two pass formats are accepted:
    #   new   -> "BIST_DONE 100kt YIELD tinter=101.2 kt GEOM tmin=158.4 ms (expect ~158.2)"
    #   legacy-> "BIST_DONE 100kt YIELD tmin=98.2 kt tinter=101.5 kt"
    # reject  -> "BIST_DONE 100kt REJECT <reason...>"  (incl. GEOM_TOLERANCE)
    # In the new form tinter is a yield (kt) and tmin is a geometric milliseconds
    # check (not a yield); in the legacy form tmin is a kt yield. Absence of
    # REJECT means pass. The preset is the token right after BIST_DONE (not the
    # last token, which is a unit suffix).
    if "BIST_DONE" in upper:
        tokens = stripped.split()
        preset = tokens[1].lower() if len(tokens) > 1 else ""
        ridx = upper.find("REJECT")
        if ridx != -1:
            reason = stripped[ridx + len("REJECT"):].strip()
            return BistDone(preset=preset, passed=False, reject_reason=reason)
        kv = _kv_tokens(stripped)
        yield_tinter = _parse_float_or_none(kv.get("tinter", "N/A"))
        if "GEOM" in upper:
            # New form: tmin= is a geometric millisecond check, not a yield.
            m = re.search(r"expect\s*~?\s*([-+0-9.eE]+)", stripped)
            expect_ms = None
            if m:
                try:
                    expect_ms = float(m.group(1))
                except ValueError:
                    expect_ms = None
            return BistDone(
                preset=preset,
                passed=True,
                yield_tinter_kt=yield_tinter,
                tmin_ms=_parse_float_or_none(kv.get("tmin", "N/A")),
                tmin_expect_ms=expect_ms,
            )
        # Legacy form: tmin= is a kt yield.
        return BistDone(
            preset=preset,
            passed=True,
            yield_tmin_kt=_parse_float_or_none(kv.get("tmin", "N/A")),
            yield_tinter_kt=yield_tinter,
        )

    # Detection reject: "REJECT NO_SECOND_FLASH" (firmware uses a space; tolerate
    # a legacy/file colon form "REJECT: <reason>" too).
    if upper.startswith("REJECT"):
        reason = stripped[len("REJECT"):].lstrip(": ").strip()
        return Reject(reason=reason)

    # Real detection: "YIELD tmin=98.2 kt tinter=101.5 kt [T_MIN_SUSPECT]".
    # (BIST_DONE lines also contain YIELD but are handled above; the DUMP header
    # keys are "YIELD_…:" and so do not match the trailing space.)
    if upper.startswith("YIELD "):
        kv = _kv_tokens(stripped)
        try:
            return YieldReport(
                yield_tmin_kt=_parse_float_or_none(kv.get("tmin", "N/A")),
                yield_tinter_kt=_parse_float_or_none(kv.get("tinter", "N/A")),
                tmin_suspect="T_MIN_SUSPECT" in upper,
            )
        except ValueError:
            return None

    # Command error reply: "ERR not_idle (DISARM first)".
    if upper.startswith("ERR ") or upper == "ERR":
        return DeviceError(message=stripped[3:].strip())

    return None

# ---------------------------------------------------------------------------
# DumpAssembler — stateful; one instance per MainWindow, reset each DUMP_START
# ---------------------------------------------------------------------------

@dataclass
class DumpAssembler:
    """Feeds on lines, emits a complete Capture when DUMP_END arrives.

    Supports two activation modes:
    - DUMP_START sentinel in the stream (the firmware emits this, as do saved
      CSV files) — the normal path.
    - arm() called externally before the stream begins, as a belt-and-braces
      pre-arm in case the DUMP_START header is ever missed.

    When SAMPLES is absent from the header, rows are accumulated in a list and
    converted to numpy arrays on DUMP_END — no pre-allocation needed.
    """

    _header: Optional[DumpHeader] = field(default=None, init=False, repr=False)
    _active: bool = field(default=False, init=False, repr=False)
    _in_edges: bool = field(default=False, init=False, repr=False)
    _header_buf: dict = field(default_factory=dict, init=False, repr=False)
    # Dynamic row accumulation used when SAMPLES is unknown
    _rows_t: list = field(default_factory=list, init=False, repr=False)
    _rows_mv: list = field(default_factory=list, init=False, repr=False)
    _edges: list = field(default_factory=list, init=False, repr=False)

    def reset(self) -> None:
        self._header = None
        self._active = False
        self._in_edges = False
        self._header_buf = {}
        self._rows_t = []
        self._rows_mv = []
        self._edges = []

    def arm(self) -> None:
        """Pre-arm for a live dump where the firmware sends no DUMP_START."""
        self.reset()
        self._active = True

    def feed(self, line: str) -> Optional[Capture]:
        """Feed one stripped line. Returns a Capture on DUMP_END, else None."""
        stripped = line.strip()

        if stripped == "DUMP_START":
            self.reset()
            self._active = True
            return None

        if not self._active:
            return None

        if stripped == "DUMP_END":
            return self._finalise()

        if stripped == "EDGES_START":
            self._in_edges = True
            return None

        if stripped == "EDGES_END":
            self._in_edges = False
            return None

        if "," in stripped and not stripped.startswith("#"):
            parts = stripped.split(",")
            if len(parts) == 2:
                if self._in_edges:
                    try:
                        self._edges.append(EdgeEvent(t_us=int(parts[0]), direction=parts[1].strip()))
                    except ValueError:
                        pass
                else:
                    # Parse both before appending either to keep arrays in sync
                    try:
                        t = int(parts[0])
                        mv = int(parts[1])
                        self._rows_t.append(t)
                        self._rows_mv.append(mv)
                    except ValueError:
                        pass
            return None

        # Header key-value pairs (before first data row, after DUMP_START)
        if self._header is None and ":" in stripped and "," not in stripped:
            key, _, val = stripped.partition(":")
            self._header_buf[key.strip()] = val.strip()

        return None

    def _finalise(self) -> Optional[Capture]:
        if not self._rows_t:
            return None

        t_us = np.array(self._rows_t, dtype=np.int64)
        adc_mv = np.array(self._rows_mv, dtype=np.int16)
        n = len(t_us)

        # Build header from buffered key-value pairs; fall back to defaults for
        # fields the firmware didn't send (sample rate derived from timestamps).
        buf = self._header_buf
        if len(t_us) > 1:
            dt_us = int(t_us[1] - t_us[0])
            derived_rate = int(1_000_000 / dt_us) if dt_us > 0 else 0
        else:
            derived_rate = 0

        header = DumpHeader(
            t0_epoch_us=int(buf.get("T0_EPOCH_US", 0)),
            sample_rate_hz=int(buf.get("SAMPLE_RATE_HZ", derived_rate)),
            samples=n,
            yield_tmin_kt=_parse_float_or_none(buf.get("YIELD_TMIN_KT", "N/A")),
            yield_tinter_kt=_parse_float_or_none(buf.get("YIELD_TINTER_KT", "N/A")),
            t_peak_to_min_ms=_parse_float_or_none(buf.get("T_PEAK_TO_MIN_MS", "N/A")),
            t_peak_to_peak_ms=_parse_float_or_none(buf.get("T_PEAK_TO_PEAK_MS", "N/A")),
        )

        cap = Capture(header=header, t_us=t_us, adc_mv=adc_mv, edges=list(self._edges))
        self.reset()
        return cap
