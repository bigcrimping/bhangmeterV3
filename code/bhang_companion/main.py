"""Entry point — QApplication setup and stylesheet load."""
from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from bhang_companion.main_window import MainWindow

_ROOT_DIR = Path(__file__).resolve().parents[2]
_ASSETS_DIR = _ROOT_DIR / "assets"
_ICON_PATH = _ASSETS_DIR / "bhang.png"
_STYLES_DIR = Path(__file__).parent / "styles"
_APP_USER_MODEL_ID = "bhang.bhangmeter.companion"


def _set_windows_app_id() -> None:
    """Tell Windows to treat this as a distinct app so the taskbar uses our
    window icon instead of the host python.exe icon."""
    if sys.platform != "win32":
        return
    import ctypes

    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
            _APP_USER_MODEL_ID
        )
    except (AttributeError, OSError):
        pass


def main() -> None:
    _set_windows_app_id()

    app = QApplication(sys.argv)
    app.setApplicationName("Bhangmeter Companion")
    app.setOrganizationName("bhang")

    # QIcon must be constructed after QApplication exists, since it builds a
    # QPixmap internally.
    app_icon = QIcon(str(_ICON_PATH)) if _ICON_PATH.exists() else QIcon()
    if not app_icon.isNull():
        app.setWindowIcon(app_icon)

    qss_path = _STYLES_DIR / "coldwar.qss"
    if qss_path.exists():
        app.setStyleSheet(qss_path.read_text(encoding="utf-8"))

    window = MainWindow()
    if not app_icon.isNull():
        window.setWindowIcon(app_icon)
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
