"""Waveform plot widget — pyqtgraph PlotWidget wrapper."""
from __future__ import annotations

import numpy as np
import pyqtgraph as pg
import pyqtgraph.exporters  # noqa: F401 — registers pg.exporters submodule
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QPushButton, QVBoxLayout, QLabel, QWidget

from bhang_companion.protocol import Capture

# Log axis floor — pyqtgraph log mode chokes on t=0
LOG_TIME_FLOOR_US = 100

_PALETTE = ["#FFA500", "#44DDFF", "#AAFF44", "#FF44DD", "#FFFF44", "#FF8844"]


class WaveformPlot(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._log_time = True
        self._captures: list[Capture] = []
        self._curves: list[pg.PlotDataItem] = []
        self._baseline_lines: list[pg.InfiniteLine] = []
        self._edge_markers: list[pg.InfiniteLine] = []

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        # Toolbar
        toolbar = QHBoxLayout()
        self._log_btn = QPushButton("Linear time")
        self._log_btn.setCheckable(True)
        self._log_btn.clicked.connect(self._toggle_log_time)
        self._clear_btn = QPushButton("Clear")
        self._clear_btn.clicked.connect(self.clear)
        self._minmax_lbl = QLabel("  Min = —   Max = —")
        self._readout = QLabel("  t = —   V = —")
        self._readout.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        toolbar.addWidget(self._log_btn)
        toolbar.addWidget(self._clear_btn)
        toolbar.addStretch()
        toolbar.addWidget(self._minmax_lbl)
        toolbar.addWidget(self._readout)
        layout.addLayout(toolbar)

        # Plot widget
        self._pw = pg.PlotWidget()
        self._pw.setBackground("#1a1a1a")
        self._pw.showGrid(x=True, y=True, alpha=0.3)
        self._pw.setLogMode(x=True, y=False)
        self._pw.setLabel("bottom", "Time (µs)")
        self._pw.setLabel("left", "Voltage (mV)")

        layout.addWidget(self._pw)

        # Crosshair readout via SignalProxy
        self._proxy = pg.SignalProxy(
            self._pw.scene().sigMouseMoved, rateLimit=60, slot=self._on_mouse_moved
        )

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def show_capture(self, cap: Capture) -> None:
        self._captures.append(cap)
        self._redraw()

    def clear(self) -> None:
        self._captures.clear()
        for c in self._curves:
            self._pw.removeItem(c)
        self._curves.clear()
        for b in self._baseline_lines:
            self._pw.removeItem(b)
        self._baseline_lines.clear()
        self._clear_edge_markers()
        self._minmax_lbl.setText("  Min = —   Max = —")
        self._readout.setText("  t = —   V = —")

    def export_png(self, path: str) -> None:
        exporter = pg.exporters.ImageExporter(self._pw.plotItem)
        exporter.export(path)

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    def _clear_edge_markers(self) -> None:
        for m in self._edge_markers:
            self._pw.removeItem(m)
        self._edge_markers.clear()

    def _toggle_log_time(self, checked: bool) -> None:
        self._log_time = not checked
        self._log_btn.setText("Linear time" if self._log_time else "Log time")
        self._pw.setLogMode(x=self._log_time, y=False)
        if self._captures:
            self._redraw()

    def _redraw(self) -> None:
        if not self._captures:
            return

        for c in self._curves:
            self._pw.removeItem(c)
        self._curves.clear()
        for b in self._baseline_lines:
            self._pw.removeItem(b)
        self._baseline_lines.clear()
        self._clear_edge_markers()

        all_t: list[np.ndarray] = []
        all_y: list[np.ndarray] = []

        for i, cap in enumerate(self._captures):
            colour = _PALETTE[i % len(_PALETTE)]

            t_rel = (cap.t_us - cap.t_us[0]).astype(np.float64)
            t = np.maximum(t_rel, LOG_TIME_FLOOR_US) if self._log_time else t_rel
            y = cap.adc_mv.astype(np.float64)

            curve = self._pw.plot(pen=pg.mkPen(colour, width=1))
            curve.setData(t, y)
            self._curves.append(curve)

            pre = max(1, len(y) // 20)
            baseline_line = pg.InfiniteLine(
                angle=0, pen=pg.mkPen(colour, width=1, style=Qt.PenStyle.DashLine)
            )
            baseline_line.setValue(float(np.mean(y[:pre])))
            self._pw.addItem(baseline_line)
            self._baseline_lines.append(baseline_line)

            t0 = int(cap.t_us[0])
            for edge in cap.edges:
                # Firmware logs ONSET (flash onset, COMP_OUT falling) and END
                # (flash end, COMP_OUT rising). Green = onset, red = end.
                edge_colour = "#44FF44" if edge.direction.upper() == "ONSET" else "#FF4444"
                t_rel_e = float(edge.t_us - t0)
                x = float(np.log10(max(t_rel_e, LOG_TIME_FLOOR_US))) if self._log_time else t_rel_e
                line = pg.InfiniteLine(
                    pos=x, angle=90,
                    pen=pg.mkPen(edge_colour, width=1, style=Qt.PenStyle.DashLine),
                    label=edge.direction,
                    labelOpts={"color": edge_colour, "position": 0.95},
                )
                self._pw.addItem(line)
                self._edge_markers.append(line)

            all_t.append(t)
            all_y.append(y)

        t_all = np.concatenate(all_t)
        y_all = np.concatenate(all_y)

        y_min, y_max = float(y_all.min()), float(y_all.max())
        self._minmax_lbl.setText(f"  Min = {y_min:.1f} mV   Max = {y_max:.1f} mV")
        y_pad = max((y_max - y_min) * 0.05, 10.0)

        if self._log_time:
            self._pw.setXRange(float(np.log10(t_all.min())), float(np.log10(t_all.max())), padding=0.05)
        else:
            self._pw.setXRange(float(t_all.min()), float(t_all.max()), padding=0.05)
        self._pw.setYRange(y_min - y_pad, y_max + y_pad)

    def _on_mouse_moved(self, event: tuple) -> None:
        pos = event[0]
        if self._pw.sceneBoundingRect().contains(pos):
            point = self._pw.getPlotItem().vb.mapSceneToView(pos)
            t_rel_us = 10 ** point.x() if self._log_time else point.x()
            self._readout.setText(f"  t = {t_rel_us/1000:.3f} ms   V = {point.y():.1f} mV")
