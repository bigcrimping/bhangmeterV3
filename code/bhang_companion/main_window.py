"""Main window — wires worker to widgets, owns DumpAssembler."""
from __future__ import annotations

import csv
import os
from pathlib import Path
from typing import Optional

from PySide6.QtCore import Qt, QThread, Signal, Slot
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import (
    QDockWidget,
    QFileDialog,
    QMainWindow,
    QWidget,
)

from bhang_companion import protocol
from bhang_companion.protocol import (
    CONFIG_MODE_BANNER,
    Capture,
    DumpAssembler,
    DumpHeader,
    StateChange,
    StatusReport,
    BistDone,
    Reject,
)
from bhang_companion.serial_worker import SerialWorker
from bhang_companion.widgets.config_dialog import ConfigDialog
from bhang_companion.widgets.console import Console
from bhang_companion.widgets.control_panel import ControlPanel
from bhang_companion.widgets.status_panel import StatusPanel
from bhang_companion.widgets.waveform_plot import WaveformPlot

_KNOWN_STATES = {"IDLE", "ARMED", "CAPTURING", "ANALYSING", "DONE", "REJECT"}


class MainWindow(QMainWindow):
    # Private signals used to marshal calls onto the worker's thread
    _sig_open_port = Signal(str, int)
    _sig_close_port = Signal()
    _sig_send = Signal(str)

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Bhangmeter Companion")
        self.resize(1280, 800)

        icon_path = Path(__file__).resolve().parents[2] / "assets" / "bhang.png"
        if icon_path.exists():
            self.setWindowIcon(QIcon(str(icon_path)))

        self._last_capture: Optional[Capture] = None
        self._assembler = DumpAssembler()
        self._local_state = "IDLE"
        self._auto_dump = False
        # "waiting" → looking for CONFIG: header; "collecting" → routing key=value lines
        self._show_state: Optional[str] = None

        # Widgets
        self._plot = WaveformPlot()
        self._control = ControlPanel()
        self._status = StatusPanel()
        self._console = Console()
        self._config_dialog = ConfigDialog(self)

        self.setCentralWidget(self._plot)
        self._add_dock("Control", self._control, Qt.DockWidgetArea.LeftDockWidgetArea)
        self._add_dock("Status", self._status, Qt.DockWidgetArea.RightDockWidgetArea)
        self._add_dock("Console", self._console, Qt.DockWidgetArea.BottomDockWidgetArea)

        # Serial worker on its own thread
        self._thread = QThread(self)
        self._worker = SerialWorker()
        self._worker.moveToThread(self._thread)
        self._thread.start()

        self._wire_signals()
        self._build_menu()

        self.statusBar().showMessage("Ready")

        # Auto-connect to a detected RP2350 USB serial port on startup; STATUS
        # is then sent automatically once the connection is established.
        if self._control.try_auto_connect():
            self.statusBar().showMessage("USB serial device found — connecting…")

    # ------------------------------------------------------------------
    # Setup
    # ------------------------------------------------------------------

    def _add_dock(self, title: str, widget: QWidget, area: Qt.DockWidgetArea) -> QDockWidget:
        dock = QDockWidget(title, self)
        dock.setWidget(widget)
        dock.setAllowedAreas(Qt.DockWidgetArea.AllDockWidgetAreas)
        self.addDockWidget(area, dock)
        return dock

    def _wire_signals(self) -> None:
        # Cross-thread signals → worker slots (QueuedConnection guarantees the
        # slot runs in the worker's event loop, not the main thread)
        self._sig_open_port.connect(self._worker.open_port, Qt.ConnectionType.QueuedConnection)
        self._sig_close_port.connect(self._worker.close_port, Qt.ConnectionType.QueuedConnection)
        self._sig_send.connect(self._worker.send, Qt.ConnectionType.QueuedConnection)

        # Worker → main thread (auto-connection is fine here; worker emits, main receives)
        self._worker.line_received.connect(self._on_line_received)
        self._worker.connection_changed.connect(self._on_connection_changed)
        self._worker.write_failed.connect(self._on_write_failed)

        # Control panel user actions — all fan through _send / private signals
        self._control.connect_requested.connect(self._sig_open_port)
        self._control.disconnect_requested.connect(self._sig_close_port)
        self._control.arm_requested.connect(lambda: self._send(protocol.cmd_arm()))
        self._control.disarm_requested.connect(lambda: self._send(protocol.cmd_disarm()))
        self._control.status_requested.connect(lambda: self._send(protocol.cmd_status()))
        self._control.bist_requested.connect(
            lambda p: self._send(protocol.cmd_bist(p))
        )
        self._control.bist_dump_requested.connect(self._on_bist_dump_requested)
        self._control.dump_requested.connect(self._on_dump_requested)
        self._control.save_capture_requested.connect(self._save_capture)

        # Config dialog
        self._config_dialog.push_requested.connect(self._on_config_push)
        self._config_dialog.pull_requested.connect(self._on_config_pull)

    def _build_menu(self) -> None:
        menu = self.menuBar()

        file_menu = menu.addMenu("&File")
        file_menu.addAction("Open CSV…", self._open_csv)
        file_menu.addAction("Save Capture…", self._save_capture)
        file_menu.addAction("Export PNG…", self._export_png)
        file_menu.addSeparator()
        file_menu.addAction("Quit", self.close)

        device_menu = menu.addMenu("&Device")
        device_menu.addAction("Device Config…", self._config_dialog.show)

        view_menu = menu.addMenu("&View")
        # Dock toggles added after docks are created — iterate children
        for dock in self.findChildren(QDockWidget):
            view_menu.addAction(dock.toggleViewAction())

    # ------------------------------------------------------------------
    # Slots
    # ------------------------------------------------------------------

    @Slot(str)
    def _on_line_received(self, line: str) -> None:
        self._console.append_rx(line)

        stripped = line.strip()

        # Auto-open config dialog on first-boot banner
        if stripped == CONFIG_MODE_BANNER:
            self._console.append_note("Config mode detected — opening config dialog")
            self._config_dialog.show()
            self._config_dialog.raise_()
            return

        # Route SHOW response lines to the config dialog
        if self._show_state == "collecting":
            result = protocol.parse_show_line(line)
            if result:
                self._config_dialog.receive_show_line(*result)
                return
            if stripped == "CONFIG:":
                return
            if stripped:
                self._show_state = None  # end of SHOW block — fall through
        elif self._show_state == "waiting" and stripped == "CONFIG:":
            self._show_state = "collecting"
            return

        # Try DumpAssembler first (feeds header/row/end lines)
        cap = self._assembler.feed(line)
        if cap is not None:
            self._on_capture_complete(cap)
            return

        # Then parse discrete messages
        msg = protocol.parse_line(line)
        if isinstance(msg, StatusReport):
            self._on_status_report(msg)
        elif isinstance(msg, StateChange):
            self._on_state_change(msg.name)
        elif isinstance(msg, BistDone):
            self._on_bist_done(msg)
        elif isinstance(msg, Reject):
            self._status.set_reject(msg.reason)
            self._console.append_note(f"REJECT: {msg.reason}")
        elif isinstance(msg, DumpHeader):
            # parse_line returns None for header lines (consumed by assembler);
            # this branch is a safety net — should not normally fire.
            self.statusBar().showMessage("Receiving DUMP…")

    @Slot(bool, str)
    def _on_connection_changed(self, connected: bool, message: str) -> None:
        self._control.set_connected(connected)
        self._status.set_connection(connected, message)
        self._config_dialog.set_connected(connected)
        self._console.append_note(message)
        self.statusBar().showMessage(message)
        if connected:
            # Query current firmware state immediately after connecting.
            self._send(protocol.cmd_status())
        else:
            self._local_state = "IDLE"
            self._show_state = None
            self._status.set_state("NOT CONNECTED")

    @Slot(str)
    def _on_write_failed(self, message: str) -> None:
        self._console.append_note(f"Write failed: {message}")
        self.statusBar().showMessage(f"Write failed: {message}")

    def _on_state_change(self, state: str) -> None:
        if state not in _KNOWN_STATES:
            self._console.append_note(f"WARNING: unrecognised state '{state}'")
            return

        if self._local_state != state:
            self._local_state = state

        self._status.set_state(state)
        self._control.set_firmware_state(state)
        self.statusBar().showMessage(f"State: {state}")

    def _on_status_report(self, report: StatusReport) -> None:
        """STATUS reply: update state plus the preset and last-known yields the
        device just reported, so the panel is populated without a DUMP."""
        self._on_state_change(report.state)
        if report.preset:
            self._status.set_preset(report.preset)
        self._status.set_yield_values(report.yield_tmin_kt, report.yield_tinter_kt)

    def _on_bist_done(self, msg: BistDone) -> None:
        """A BIST result line. Drives the 'Fire + Dump' auto-dump directly (a
        rejected BIST never reaches a DONE state, and an armed pass re-arms
        rather than going DONE — so keying off state here is unreliable)."""
        self._status.set_last_bist(msg.preset)
        if msg.passed:
            self._status.set_yield_values(msg.yield_tmin_kt, msg.yield_tinter_kt)
            self._console.append_note(f"BIST {msg.preset} PASS")
        else:
            self._status.set_reject(msg.reject_reason or "(no reason)")
            self._console.append_note(f"BIST {msg.preset} REJECT: {msg.reject_reason}")

        if self._auto_dump:
            self._auto_dump = False
            if msg.passed:
                self._on_dump_requested()

    def _on_capture_complete(self, cap: Capture) -> None:
        self._last_capture = cap
        self._plot.show_capture(cap)
        self._status.set_yields(cap.header)
        self._control.enable_save(True)
        n = len(cap.t_us)
        self.statusBar().showMessage(f"Capture received — {n} samples")

    # ------------------------------------------------------------------
    # Config actions
    # ------------------------------------------------------------------

    @Slot(dict)
    def _on_config_push(self, cfg: dict) -> None:
        for key, value in cfg.items():
            if value.strip():
                self._send(protocol.cmd_set(key, value))
        self._send(protocol.cmd_disarm())
        self._send(protocol.cmd_save())
        self.statusBar().showMessage("Config pushed — waiting for OK SAVED…")

    @Slot()
    def _on_config_pull(self) -> None:
        self._show_state = "waiting"
        self._send(protocol.cmd_show())
        self.statusBar().showMessage("Reading config from device…")

    # ------------------------------------------------------------------
    # File actions
    # ------------------------------------------------------------------

    def _on_bist_dump_requested(self, preset: str) -> None:
        self._auto_dump = True
        self._send(protocol.cmd_bist(preset))

    def _on_dump_requested(self) -> None:
        # Belt-and-braces pre-arm; the firmware's DUMP_START sentinel resets the
        # assembler again when it arrives, so this just covers a missed header.
        self._assembler.arm()
        self.statusBar().showMessage("Receiving DUMP…")
        self._send(protocol.cmd_dump())

    def _send(self, cmd: str) -> None:
        self._console.append_tx(cmd)
        self._sig_send.emit(cmd)  # queued — runs in worker thread

    def _open_csv(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Open DUMP CSV", "", "CSV/text files (*.csv *.txt);;All files (*)"
        )
        if not path:
            return
        self._assembler.reset()
        try:
            with open(path, encoding="utf-8") as f:
                for raw_line in f:
                    line = raw_line.rstrip("\r\n")
                    self._console.append_rx(line)
                    cap = self._assembler.feed(line)
                    if cap is not None:
                        self._on_capture_complete(cap)
                        return
            self.statusBar().showMessage(f"Loaded {os.path.basename(path)} (no DUMP_END found)")
        except OSError as e:
            self.statusBar().showMessage(f"Failed to open {path}: {e}")

    def _save_capture(self) -> None:
        if not self._last_capture:
            self.statusBar().showMessage("No capture to save")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Capture", "", "CSV files (*.csv);;All files (*)"
        )
        if not path:
            return
        cap = self._last_capture
        h = cap.header
        with open(path, "w", newline="", encoding="utf-8") as f:
            f.write("DUMP_START\n")
            f.write(f"T0_EPOCH_US: {h.t0_epoch_us}\n")
            f.write(f"SAMPLE_RATE_HZ: {h.sample_rate_hz}\n")
            f.write(f"SAMPLES: {h.samples}\n")
            tmin = f"{h.yield_tmin_kt:.2f}" if h.yield_tmin_kt is not None else "N/A"
            tinter = f"{h.yield_tinter_kt:.2f}" if h.yield_tinter_kt is not None else "N/A"
            f.write(f"YIELD_TMIN_KT: {tmin}\n")
            f.write(f"YIELD_TINTER_KT: {tinter}\n")
            tpm = f"{h.t_peak_to_min_ms:.2f}" if h.t_peak_to_min_ms is not None else "N/A"
            tpp = f"{h.t_peak_to_peak_ms:.2f}" if h.t_peak_to_peak_ms is not None else "N/A"
            f.write(f"T_PEAK_TO_MIN_MS: {tpm}\n")
            f.write(f"T_PEAK_TO_PEAK_MS: {tpp}\n")
            writer = csv.writer(f)
            for t, mv in zip(cap.t_us, cap.adc_mv):
                writer.writerow([t, mv])
            if cap.edges:
                f.write("EDGES_START\n")
                for e in cap.edges:
                    f.write(f"{e.t_us},{e.direction}\n")
                f.write("EDGES_END\n")
            f.write("DUMP_END\n")
        self.statusBar().showMessage(f"Saved to {os.path.basename(path)}")

    def _export_png(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Export PNG", "", "PNG images (*.png)"
        )
        if path:
            self._plot.export_png(path)
            self.statusBar().showMessage(f"Exported {os.path.basename(path)}")

    # ------------------------------------------------------------------

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self._sig_close_port.emit()  # queued — runs in worker thread
        self._thread.quit()
        self._thread.wait(2000)
        super().closeEvent(event)
