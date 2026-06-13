"""Entry point — QApplication setup and stylesheet load."""
from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtWidgets import QApplication

from bhang_companion.main_window import MainWindow

_STYLES_DIR = Path(__file__).parent / "styles"


def main() -> None:
    app = QApplication(sys.argv)
    app.setApplicationName("Bhangmeter Companion")
    app.setOrganizationName("bhang")

    qss_path = _STYLES_DIR / "coldwar.qss"
    if qss_path.exists():
        app.setStyleSheet(qss_path.read_text(encoding="utf-8"))

    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
