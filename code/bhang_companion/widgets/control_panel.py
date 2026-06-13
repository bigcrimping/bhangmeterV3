"""Control panel — connection, arm/disarm, BIST, capture."""
from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from bhang_companion.protocol import PRESETS
from bhang_companion.serial_worker import SerialWorker

_BAUDS = [1_000_000, 250_000, 921600, 115200]
_BAUD_USB_TIP = (
    "RP2350 USB CDC ignores this value — it runs at native USB speed.\n"
    "Only relevant when using a UART-to-USB debug probe (e.g. Picoprobe)."
)


class ControlPanel(QWidget):
    # Signals emitted to MainWindow
    connect_requested = Signal(str, int)   # port, baud
    disconnect_requested = Signal()
    arm_requested = Signal()
    disarm_requested = Signal()
    status_requested = Signal()
    bist_requested = Signal(str)           # preset name
    bist_dump_requested = Signal(str)      # preset name — fire then auto-dump on DONE
    dump_requested = Signal()
    save_capture_requested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(6)

        layout.addWidget(self._build_connection_group())
        layout.addWidget(self._build_arm_group())
        layout.addWidget(self._build_bist_group())
        layout.addWidget(self._build_capture_group())
        layout.addStretch()

        self._set_connected(False)

    # ------------------------------------------------------------------
    # Group builders
    # ------------------------------------------------------------------

    def _build_connection_group(self) -> QGroupBox:
        box = QGroupBox("Connection")
        layout = QVBoxLayout(box)

        port_row = QHBoxLayout()
        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(120)
        self._refresh_btn = QToolButton()
        self._refresh_btn.setText("⟳")
        self._refresh_btn.setToolTip("Refresh port list")
        self._refresh_btn.clicked.connect(self.refresh_ports)
        port_row.addWidget(QLabel("Port:"))
        port_row.addWidget(self._port_combo, 1)
        port_row.addWidget(self._refresh_btn)

        baud_row = QHBoxLayout()
        self._baud_combo = QComboBox()
        for b in _BAUDS:
            self._baud_combo.addItem(str(b), b)
        self._baud_combo.setToolTip(_BAUD_USB_TIP)
        baud_row.addWidget(QLabel("Baud:"))
        baud_row.addWidget(self._baud_combo, 1)

        btn_row = QHBoxLayout()
        self._connect_btn = QPushButton("Connect")
        self._disconnect_btn = QPushButton("Disconnect")
        self._connect_btn.clicked.connect(self._on_connect_clicked)
        self._disconnect_btn.clicked.connect(self.disconnect_requested)
        btn_row.addWidget(self._connect_btn)
        btn_row.addWidget(self._disconnect_btn)

        layout.addLayout(port_row)
        layout.addLayout(baud_row)
        layout.addLayout(btn_row)
        self.refresh_ports()
        return box

    def _build_arm_group(self) -> QGroupBox:
        box = QGroupBox("Arm")
        layout = QHBoxLayout(box)
        self._arm_btn = QPushButton("ARM")
        self._disarm_btn = QPushButton("DISARM")
        self._status_btn = QPushButton("STATUS")
        self._arm_btn.clicked.connect(self.arm_requested)
        self._disarm_btn.clicked.connect(self.disarm_requested)
        self._status_btn.clicked.connect(self.status_requested)
        layout.addWidget(self._arm_btn)
        layout.addWidget(self._disarm_btn)
        layout.addWidget(self._status_btn)
        return box

    def _build_bist_group(self) -> QGroupBox:
        box = QGroupBox("BIST")
        layout = QVBoxLayout(box)

        preset_row = QHBoxLayout()
        self._preset_combo = QComboBox()
        for p in PRESETS:
            self._preset_combo.addItem(p)
        preset_row.addWidget(QLabel("Preset:"))
        preset_row.addWidget(self._preset_combo, 1)

        btn_row = QHBoxLayout()
        self._bist_btn = QPushButton("Fire")
        self._bist_dump_btn = QPushButton("Fire + Dump")
        self._bist_btn.clicked.connect(self._on_bist_clicked)
        self._bist_dump_btn.clicked.connect(self._on_bist_dump_clicked)
        btn_row.addWidget(self._bist_btn)
        btn_row.addWidget(self._bist_dump_btn)

        layout.addLayout(preset_row)
        layout.addLayout(btn_row)
        return box

    def _build_capture_group(self) -> QGroupBox:
        box = QGroupBox("Capture")
        layout = QHBoxLayout(box)
        self._dump_btn = QPushButton("DUMP")
        self._save_btn = QPushButton("Save CSV")
        self._dump_btn.clicked.connect(self.dump_requested)
        self._save_btn.clicked.connect(self.save_capture_requested)
        layout.addWidget(self._dump_btn)
        layout.addWidget(self._save_btn)
        return box

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    def refresh_ports(self) -> None:
        self._port_combo.clear()
        preferred_idx = -1
        self._preferred_device: str | None = None
        for device, desc in SerialWorker.list_ports():
            label = desc if desc and desc != device else device
            self._port_combo.addItem(label, device)
            # The RP2350 enumerates on Windows as "USB Serial Device" — prefer
            # the last such port (the debugger enumerates on a lower-numbered
            # port) so the user usually need not pick manually.
            if "usb serial device" in (desc or "").lower():
                preferred_idx = self._port_combo.count() - 1
                self._preferred_device = device
        if self._port_combo.count() == 0:
            self._port_combo.addItem("(no ports found)", "")
        elif preferred_idx >= 0:
            self._port_combo.setCurrentIndex(preferred_idx)

    def try_auto_connect(self) -> bool:
        """Request a connection to the auto-detected USB serial port, if any.

        Returns True when a connect was requested. The selected port/baud are
        used, which refresh_ports has already pointed at the preferred device.
        """
        if getattr(self, "_preferred_device", None):
            baud = self._baud_combo.currentData() or 1_000_000
            self.connect_requested.emit(self._preferred_device, baud)
            return True
        return False

    def set_firmware_state(self, state: str) -> None:
        """Enable/disable buttons based on firmware state (ground truth)."""
        s = state.upper()
        self._arm_btn.setEnabled(s in ("IDLE", "DONE"))
        self._disarm_btn.setEnabled(s in ("ARMED",))
        self._bist_btn.setEnabled(s in ("IDLE", "DONE"))
        self._bist_dump_btn.setEnabled(s in ("IDLE", "DONE"))
        self._dump_btn.setEnabled(True)  # always available — firmware stores capture on failure too

    def set_connected(self, connected: bool) -> None:
        self._set_connected(connected)

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    def _set_connected(self, connected: bool) -> None:
        self._connect_btn.setEnabled(not connected)
        self._disconnect_btn.setEnabled(connected)
        self._arm_btn.setEnabled(False)
        self._disarm_btn.setEnabled(False)
        self._status_btn.setEnabled(connected)
        self._bist_btn.setEnabled(False)
        self._bist_dump_btn.setEnabled(False)
        self._dump_btn.setEnabled(connected)
        self._save_btn.setEnabled(False)

    def _on_connect_clicked(self) -> None:
        device = self._port_combo.currentData() or self._port_combo.currentText()
        baud = self._baud_combo.currentData() or 1_000_000
        if device:
            self.connect_requested.emit(device, baud)

    def _on_bist_clicked(self) -> None:
        self.bist_requested.emit(self._preset_combo.currentText())

    def _on_bist_dump_clicked(self) -> None:
        self.bist_dump_requested.emit(self._preset_combo.currentText())

    def enable_save(self, enabled: bool) -> None:
        self._save_btn.setEnabled(enabled)
