"""Status panel — read-only display of firmware state and capture metadata."""
from __future__ import annotations

import datetime

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QFormLayout,
    QFrame,
    QGroupBox,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from bhang_companion.protocol import DumpHeader

_STATE_COLOURS: dict[str, str] = {
    "NOT CONNECTED": "#666666",
    "IDLE":          "#888888",
    "ARMED":         "#FFA500",
    "CAPTURING":     "#FF4444",
    "ANALYSING":     "#FFDD44",
    "DONE":          "#44CC44",
    "REJECT":        "#FF4444",
}


def _label(text: str = "—") -> QLabel:
    lbl = QLabel(text)
    lbl.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
    return lbl


class StatusPanel(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)

        # ---- State indicator ----
        self._state_lbl = QLabel("NOT CONNECTED")
        self._state_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        font = self._state_lbl.font()
        font.setPointSize(18)
        font.setBold(True)
        self._state_lbl.setFont(font)
        self._state_lbl.setFrameShape(QFrame.Shape.StyledPanel)
        self._state_lbl.setMinimumHeight(50)
        layout.addWidget(self._state_lbl)
        self._apply_state_colour("NOT CONNECTED")

        # ---- Form fields ----
        box = QGroupBox("Capture Info")
        form = QFormLayout(box)
        form.setLabelAlignment(Qt.AlignmentFlag.AlignRight)

        self._preset_lbl = _label()
        self._bist_lbl = _label()
        self._reject_lbl = _label()
        self._yield_tmin_lbl = _label()
        self._yield_tinter_lbl = _label()
        self._t_min_lbl = _label()
        self._t_inter_lbl = _label()
        self._samples_lbl = _label()
        self._fw_time_lbl = _label()
        self._host_time_lbl = _label()
        self._conn_lbl = _label("Disconnected")

        form.addRow("Preset:", self._preset_lbl)
        form.addRow("Last BIST:", self._bist_lbl)
        form.addRow("Last reject:", self._reject_lbl)
        form.addRow("Yield (T_min):", self._yield_tmin_lbl)
        form.addRow("Yield (T_inter):", self._yield_tinter_lbl)
        form.addRow("T_min (peak→min):", self._t_min_lbl)
        form.addRow("T_inter (peak→peak):", self._t_inter_lbl)
        form.addRow("Samples:", self._samples_lbl)
        form.addRow("FW timestamp:", self._fw_time_lbl)
        form.addRow("Host received:", self._host_time_lbl)
        form.addRow("Connection:", self._conn_lbl)

        layout.addWidget(box)
        layout.addStretch()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def set_state(self, state: str) -> None:
        self._state_lbl.setText(state)
        self._apply_state_colour(state)

    def set_preset(self, preset: str) -> None:
        self._preset_lbl.setText(preset)

    def set_last_bist(self, preset: str) -> None:
        self._bist_lbl.setText(preset)

    def set_reject(self, reason: str) -> None:
        self._reject_lbl.setText(reason)

    def set_yield_values(self, tmin_kt: float | None, tinter_kt: float | None) -> None:
        def fmt(v: float | None) -> str:
            return f"{v:.1f} kt" if v is not None else "N/A"

        self._yield_tmin_lbl.setText(fmt(tmin_kt))
        self._yield_tinter_lbl.setText(fmt(tinter_kt))

    def set_timing(self, tmin_ms: float | None, tinter_ms: float | None) -> None:
        def fmt(v: float | None) -> str:
            return f"{v:.1f} ms" if v is not None else "N/A"

        self._t_min_lbl.setText(fmt(tmin_ms))
        self._t_inter_lbl.setText(fmt(tinter_ms))

    def set_yields(self, header: DumpHeader) -> None:
        self.set_yield_values(header.yield_tmin_kt, header.yield_tinter_kt)
        self.set_timing(header.t_peak_to_min_ms, header.t_peak_to_peak_ms)
        self._samples_lbl.setText(str(header.samples))

        epoch_us = header.t0_epoch_us
        if epoch_us > 0:
            dt = datetime.datetime.fromtimestamp(epoch_us / 1_000_000, tz=datetime.timezone.utc)
            self._fw_time_lbl.setText(dt.strftime("%Y-%m-%d %H:%M:%S.%f UTC"))
        else:
            self._fw_time_lbl.setText("(no epoch)")

        now = datetime.datetime.now(tz=datetime.timezone.utc)
        self._host_time_lbl.setText(now.strftime("%Y-%m-%d %H:%M:%S UTC"))

    def set_connection(self, connected: bool, message: str) -> None:
        self._conn_lbl.setText(message)

    # ------------------------------------------------------------------

    def _apply_state_colour(self, state: str) -> None:
        colour = _STATE_COLOURS.get(state.upper(), "#888888")
        self._state_lbl.setStyleSheet(
            f"QLabel {{ color: {colour}; border: 2px solid {colour}; "
            f"border-radius: 4px; padding: 4px; }}"
        )
