"""Raw serial log console — append-only, monospace, max-line capped."""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QHBoxLayout,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
    QFileDialog,
)

_MAX_LINES = 5000


class Console(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        # Toolbar
        bar = QHBoxLayout()
        self._autoscroll_btn = QPushButton("Autoscroll ON")
        self._autoscroll_btn.setCheckable(True)
        self._autoscroll_btn.setChecked(True)
        self._autoscroll_btn.clicked.connect(self._toggle_autoscroll)
        self._clear_btn = QPushButton("Clear")
        self._clear_btn.clicked.connect(self._text.clear if hasattr(self, "_text") else lambda: None)
        self._save_btn = QPushButton("Save log…")
        self._save_btn.clicked.connect(self._save_log)
        bar.addWidget(self._autoscroll_btn)
        bar.addWidget(self._clear_btn)
        bar.addStretch()
        bar.addWidget(self._save_btn)
        layout.addLayout(bar)

        self._text = QPlainTextEdit()
        self._text.setReadOnly(True)
        font = QFont("Consolas", 9)
        font.setStyleHint(QFont.StyleHint.Monospace)
        self._text.setFont(font)
        self._text.setMaximumBlockCount(_MAX_LINES)
        layout.addWidget(self._text)

        # Wire clear now that _text exists
        self._clear_btn.clicked.disconnect()
        self._clear_btn.clicked.connect(self._text.clear)

    # ------------------------------------------------------------------
    # Public
    # ------------------------------------------------------------------

    def append_rx(self, line: str) -> None:
        """Append an incoming (received) line."""
        self._append(f"< {line}")

    def append_tx(self, line: str) -> None:
        """Append an outgoing (sent) command."""
        self._append(f"> {line.strip()}")

    def append_note(self, text: str) -> None:
        """Append a local status/warning note."""
        self._append(f"  [{text}]")

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    def _append(self, text: str) -> None:
        self._text.appendPlainText(text)
        if self._autoscroll_btn.isChecked():
            self._text.moveCursor(QTextCursor.MoveOperation.End)

    def _toggle_autoscroll(self, checked: bool) -> None:
        self._autoscroll_btn.setText("Autoscroll ON" if checked else "Autoscroll OFF")

    def _save_log(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Save log", "", "Log files (*.log);;All files (*)")
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self._text.toPlainText())
