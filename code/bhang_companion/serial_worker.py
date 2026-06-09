"""Serial I/O worker — bytes in, lines out. No parsing, no blocking."""
from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QObject, QThread, Signal, Slot
from PySide6.QtSerialPort import QSerialPort, QSerialPortInfo

try:
    from serial.tools import list_ports as _list_ports
    _HAS_PYSERIAL = True
except ImportError:
    _HAS_PYSERIAL = False


class SerialWorker(QObject):
    """Owns one QSerialPort. Emits one signal per complete UTF-8 line.

    RP2350 USB CDC ignores the baud rate and runs at native USB speed;
    the baud dropdown in the UI is provided for users connecting via a
    UART-to-USB debug probe (e.g. Picoprobe on UART0) who do need 115200.
    """

    line_received = Signal(str)             # one stripped line, no \\r\\n
    connection_changed = Signal(bool, str)  # (connected, human-readable message)
    write_failed = Signal(str)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._port: Optional[QSerialPort] = None
        self._buf = bytearray()

    # ------------------------------------------------------------------
    # Slots
    # ------------------------------------------------------------------

    @Slot(str, int)
    def open_port(self, name: str, baud: int) -> None:
        if self._port and self._port.isOpen():
            self._port.close()

        port = QSerialPort(self)
        port.setPortName(name)
        port.setBaudRate(baud)
        port.setDataBits(QSerialPort.DataBits.Data8)
        port.setParity(QSerialPort.Parity.NoParity)
        port.setStopBits(QSerialPort.StopBits.OneStop)
        port.setFlowControl(QSerialPort.FlowControl.NoFlowControl)
        port.readyRead.connect(self._on_ready_read)
        port.errorOccurred.connect(self._on_error)

        if port.open(QSerialPort.OpenModeFlag.ReadWrite):
            # RP2350 USB CDC (pico stdio_usb) gates all output on the DTR
            # "terminal ready" line: when DTR is low the firmware silently
            # drops TX, so commands get no echo or reply. Qt does not reliably
            # assert DTR on the first open after enumeration, so force it (and
            # RTS) high to mark the terminal as connected.
            port.setDataTerminalReady(True)
            port.setRequestToSend(True)
            self._port = port
            self._buf.clear()
            self.connection_changed.emit(True, f"Connected to {name} @ {baud} baud")
        else:
            err = port.errorString()
            port.deleteLater()
            self.connection_changed.emit(False, f"Failed to open {name}: {err}")

    @Slot()
    def close_port(self) -> None:
        if self._port:
            self._port.close()
            self._port.deleteLater()
            self._port = None
            self._buf.clear()
            self.connection_changed.emit(False, "Port closed")

    @Slot(str)
    def send(self, payload: str) -> None:
        if not self._port or not self._port.isOpen():
            self.write_failed.emit("Port not open")
            return
        data = payload if payload.endswith("\n") else payload + "\n"
        written = self._port.write(data.encode("utf-8"))
        if written < 0:
            self.write_failed.emit(f"Write error: {self._port.errorString()}")

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    @Slot()
    def _on_ready_read(self) -> None:
        if not self._port:
            return
        self._buf.extend(self._port.readAll().data())
        while b"\n" in self._buf:
            idx = self._buf.index(b"\n")
            raw = self._buf[:idx]
            self._buf = self._buf[idx + 1:]
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            self.line_received.emit(line)

    @Slot(QSerialPort.SerialPortError)
    def _on_error(self, error: QSerialPort.SerialPortError) -> None:
        if error == QSerialPort.SerialPortError.NoError:
            return
        msg = self._port.errorString() if self._port else "Unknown error"
        self.connection_changed.emit(False, f"Serial error: {msg}")
        if self._port:
            self._port.close()

    # ------------------------------------------------------------------
    # Static helpers
    # ------------------------------------------------------------------

    @staticmethod
    def list_ports() -> list[tuple[str, str]]:
        """Return [(device_path, friendly_description), ...].

        Uses pyserial for richer descriptions when available; falls back to
        Qt's own QSerialPortInfo. On Linux, CDC ACM descriptions are often
        empty — callers should display device_path verbatim in that case.
        """
        if _HAS_PYSERIAL:
            results = []
            for p in _list_ports.comports():
                desc = p.description or p.device
                results.append((p.device, desc))
            return results

        results = []
        for info in QSerialPortInfo.availablePorts():
            device = info.portName()
            desc = info.description() or device
            results.append((device, desc))
        return results
