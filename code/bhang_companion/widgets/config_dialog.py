"""Configuration dialog — read and write device config over serial."""
from __future__ import annotations

import json
from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from bhang_companion.protocol import CONFIG_KEYS, SECRET_KEYS

_LABELS = {
    "wifi_ssid":  "WiFi SSID",
    "wifi_pass":  "WiFi Password",
    "device_id":  "Device ID",
    "lat":        "Latitude",
    "lon":        "Longitude",
    "gh_user":    "GitHub User",
    "gh_repo":    "GitHub Repo",
    "gh_branch":  "GitHub Branch",
    "gh_file":    "GitHub File",
    "gh_token":   "GitHub Token",
    "ntp":        "NTP Server",
}


def _make_toggle(edit: QLineEdit, btn: QToolButton):
    def _toggle(checked: bool) -> None:
        edit.setEchoMode(QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password)
        btn.setText("Hide" if checked else "Show")
    return _toggle


class ConfigDialog(QDialog):
    """Non-modal dialog for reading and writing the device's flash config."""

    push_requested = Signal(dict)  # {key: value} — main window sends SET…DISARM…SAVE
    pull_requested = Signal()      # main window sends SHOW; routes lines back via receive_show_line

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Device Configuration")
        self.setMinimumWidth(460)

        self._fields: dict[str, QLineEdit] = {}

        outer = QVBoxLayout(self)
        outer.addWidget(self._build_form())
        outer.addLayout(self._build_action_row())
        outer.addWidget(self._build_close_button())

        self.set_connected(False)

    # ------------------------------------------------------------------
    # Build helpers
    # ------------------------------------------------------------------

    def _build_form(self) -> QGroupBox:
        box = QGroupBox("Configuration Fields")
        form = QFormLayout(box)
        form.setLabelAlignment(Qt.AlignmentFlag.AlignRight)

        for key in CONFIG_KEYS:
            label = _LABELS.get(key, key)
            edit = QLineEdit(box)

            if key in SECRET_KEYS:
                edit.setEchoMode(QLineEdit.EchoMode.Password)
                toggle = QToolButton(box)
                toggle.setText("Show")
                toggle.setCheckable(True)
                toggle.toggled.connect(_make_toggle(edit, toggle))
                row_widget = QHBoxLayout()
                row_widget.setContentsMargins(0, 0, 0, 0)
                row_widget.addWidget(edit)
                row_widget.addWidget(toggle)
                form.addRow(label + ":", row_widget)
            else:
                form.addRow(label + ":", edit)

            self._fields[key] = edit

        return box

    def _build_action_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        self._load_btn = QPushButton("Load from file…")
        self._save_file_btn = QPushButton("Save to file…")
        self._pull_btn = QPushButton("Read from device")
        self._push_btn = QPushButton("Push to device")
        self._load_btn.clicked.connect(self._load_file)
        self._save_file_btn.clicked.connect(self._save_file)
        self._pull_btn.clicked.connect(self.pull_requested)
        self._push_btn.clicked.connect(self._on_push_clicked)
        row.addWidget(self._load_btn)
        row.addWidget(self._save_file_btn)
        row.addWidget(self._pull_btn)
        row.addWidget(self._push_btn)
        return row

    def _build_close_button(self) -> QDialogButtonBox:
        box = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        box.rejected.connect(self.reject)
        return box

    # ------------------------------------------------------------------
    # Public API — called by MainWindow
    # ------------------------------------------------------------------

    def receive_show_line(self, key: str, value: str) -> None:
        """Populate a non-secret field from a parsed SHOW response line."""
        if key in SECRET_KEYS:
            return  # masked on device; keep whatever the user typed
        edit = self._fields.get(key)
        if edit is not None:
            edit.setText(value)

    def set_connected(self, connected: bool) -> None:
        self._pull_btn.setEnabled(connected)
        self._push_btn.setEnabled(connected)

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    def _config_dict(self) -> dict[str, str]:
        return {k: self._fields[k].text() for k in CONFIG_KEYS}

    def _populate(self, data: dict) -> None:
        for key in CONFIG_KEYS:
            edit = self._fields.get(key)
            if edit is not None:
                edit.setText(str(data.get(key, "")))

    def _on_push_clicked(self) -> None:
        cfg = self._config_dict()
        empty = [k for k, v in cfg.items() if not v.strip()]
        if empty:
            reply = QMessageBox.question(
                self,
                "Empty fields",
                f"These fields are empty:\n{', '.join(empty)}\n\nPush anyway?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        self.push_requested.emit(cfg)

    def _load_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Load Config", "", "JSON files (*.json);;All files (*)"
        )
        if not path:
            return
        try:
            self._populate(json.loads(Path(path).read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Load failed", str(e))

    def _save_file(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Config", "bhang_config.json", "JSON files (*.json);;All files (*)"
        )
        if not path:
            return
        try:
            Path(path).write_text(
                json.dumps(self._config_dict(), indent=2),
                encoding="utf-8",
            )
        except OSError as e:
            QMessageBox.warning(self, "Save failed", str(e))
