#!/usr/bin/env python3
"""Cambyses Scheduler Configuration Tool - PyQt5 GUI"""

import sys
import os
import subprocess
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QSlider, QFrame, QPushButton, QListWidget, QListWidgetItem,
    QGroupBox, QGridLayout, QMessageBox, QSizePolicy, QGraphicsDropShadowEffect,
    QAbstractItemView, QToolTip,
)
from PyQt5.QtCore import Qt, QMimeData, pyqtSignal, pyqtProperty, QSize, QPropertyAnimation, QEasingCurve
from PyQt5.QtGui import QDrag, QFont, QColor, QPalette, QPixmap, QPainter, QBrush, QPen

SYSCTL_ENABLED = "/proc/sys/kernel/sched_cambyses"
SYSCTL_CONFIG = "/proc/sys/kernel/sched_cambyses_config"

SIGNALS = [
    {
        "index": 0, "name": "exec_start delta", "sign": "+", "color": "#4FC3F7",
        "desc": "How long since this task last ran on CPU",
        "detail": "log₂(rq_clock − exec_start)",
        "range": "40–130 (fp2)", "typical": "~80–110",
        "recommend": "+ (promote starved tasks for migration)",
    },
    {
        "index": 1, "name": "runnable starvation", "sign": "+", "color": "#81C784",
        "desc": "Time spent waiting in runqueue vs actual execution",
        "detail": "(runnable_avg − util_avg) >> 4",
        "range": "0–64", "typical": "0–20",
        "recommend": "+ (prioritize tasks squeezed by contention)",
    },
    {
        "index": 2, "name": "io_boundness", "sign": "+", "color": "#FFB74D",
        "desc": "I/O intensity — voluntary ctx-switch ratio × idle fraction",
        "detail": "vol_ratio × (1 − util_frac) × 64",
        "range": "0–64", "typical": "10–50 for I/O tasks, ~0 for CPU-bound",
        "recommend": "+ (I/O tasks have low cache footprint, cheap to migrate)",
    },
    {
        "index": 3, "name": "wakee_penalty", "sign": "−", "color": "#E57373",
        "desc": "Wakeup relationship complexity (wakee_flips)",
        "detail": "log₂(wakee_flips + 1)",
        "range": "0–128 (fp2)", "typical": "0–40",
        "recommend": "− (penalize tasks with complex wake chains)",
    },
    {
        "index": 4, "name": "last_migrate delta", "sign": "+", "color": "#BA68C8",
        "desc": "Time since this task was last migrated between CPUs",
        "detail": "log₂(rq_clock − last_migrate)",
        "range": "40–130 (fp2)", "typical": "~80–120",
        "recommend": "+ (prefer stable tasks that haven't moved recently)",
    },
    {
        "index": 5, "name": "nvcsw ratio", "sign": "−", "color": "#F06292",
        "desc": "Fraction of voluntary context switches (sleep/I/O rate)",
        "detail": "nvcsw / (nvcsw + nivcsw) × 64",
        "range": "0–64", "typical": "~50–64 for I/O, ~0–10 for CPU-bound",
        "recommend": "− (high-sleep tasks have strong wake affinity)",
    },
    {
        "index": 6, "name": "util_avg", "sign": "+", "color": "#4DB6AC",
        "desc": "Raw CPU utilization from PELT",
        "detail": "util_avg >> 4",
        "range": "0–64", "typical": "10–60",
        "recommend": "+ (prefer heavier tasks to resolve imbalance faster)",
    },
    {
        "index": 7, "name": "weighted_load", "sign": "+", "color": "#DCE775",
        "desc": "CPU load contribution weighted by task priority",
        "detail": "(util_avg × log₂(weight)) >> 10",
        "range": "0–66", "typical": "5–40",
        "recommend": "+ (heavy tasks consume imbalance budget efficiently)",
    },
]

DARK_BG = "#1e1e2e"
DARK_SURFACE = "#2a2a3c"
DARK_SURFACE2 = "#33334a"
DARK_BORDER = "#44445a"
ACCENT = "#89b4fa"
ACCENT_DIM = "#5580c0"
TEXT_PRIMARY = "#cdd6f4"
TEXT_SECONDARY = "#a6adc8"
TEXT_MUTED = "#6c7086"
RED = "#f38ba8"
GREEN = "#a6e3a1"

STYLESHEET = f"""
QMainWindow {{
    background-color: {DARK_BG};
}}
QWidget {{
    color: {TEXT_PRIMARY};
    font-family: 'Inter', 'Segoe UI', 'Noto Sans', sans-serif;
    font-size: 13px;
}}
QGroupBox {{
    background-color: {DARK_SURFACE};
    border: 1px solid {DARK_BORDER};
    border-radius: 12px;
    margin-top: 20px;
    padding: 16px 12px 12px 12px;
    font-weight: 600;
    font-size: 14px;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 4px 14px;
    background-color: {DARK_SURFACE2};
    border: 1px solid {DARK_BORDER};
    border-radius: 8px;
    color: {ACCENT};
}}
QLabel {{
    background: transparent;
    border: none;
}}
QPushButton {{
    background-color: {DARK_SURFACE2};
    border: 1px solid {DARK_BORDER};
    border-radius: 8px;
    padding: 8px 20px;
    font-weight: 600;
    color: {TEXT_PRIMARY};
    min-height: 20px;
}}
QPushButton:hover {{
    background-color: {ACCENT_DIM};
    border-color: {ACCENT};
    color: white;
}}
QPushButton:pressed {{
    background-color: {ACCENT};
}}
QPushButton:disabled {{
    background-color: {DARK_SURFACE};
    border-color: {DARK_SURFACE2};
    color: {TEXT_MUTED};
}}
QListWidget {{
    background-color: {DARK_SURFACE};
    border: 1px solid {DARK_BORDER};
    border-radius: 10px;
    padding: 6px;
    outline: none;
}}
QListWidget::item {{
    background-color: {DARK_SURFACE2};
    border: 1px solid {DARK_BORDER};
    border-radius: 8px;
    padding: 8px 12px;
    margin: 3px 2px;
    color: {TEXT_PRIMARY};
}}
QListWidget::item:hover {{
    border-color: {ACCENT_DIM};
    background-color: #3a3a55;
}}
QListWidget::item:selected {{
    background-color: {ACCENT_DIM};
    border-color: {ACCENT};
    color: white;
}}
QSlider::groove:horizontal {{
    height: 6px;
    background: {DARK_BORDER};
    border-radius: 3px;
}}
QSlider::handle:horizontal {{
    background: {ACCENT};
    border: 2px solid {ACCENT_DIM};
    width: 18px;
    height: 18px;
    margin: -7px 0;
    border-radius: 10px;
}}
QSlider::handle:horizontal:hover {{
    background: white;
    border-color: {ACCENT};
}}
QSlider::sub-page:horizontal {{
    background: {ACCENT_DIM};
    border-radius: 3px;
}}
QToolTip {{
    background-color: {DARK_SURFACE2};
    color: {TEXT_PRIMARY};
    border: 1px solid {DARK_BORDER};
    border-radius: 6px;
    padding: 6px 10px;
    font-size: 12px;
}}
"""

APPLY_STYLE_CLEAN = f"""
    QPushButton#applyBtn {{
        background-color: {ACCENT_DIM};
        border: 1px solid {ACCENT};
        border-radius: 8px;
        color: white;
        font-size: 14px;
        font-weight: 600;
        padding: 10px 32px;
        min-height: 20px;
    }}
    QPushButton#applyBtn:hover {{
        background-color: {ACCENT};
    }}
    QPushButton#applyBtn:disabled {{
        background-color: {DARK_SURFACE};
        border-color: {DARK_SURFACE2};
        color: {TEXT_MUTED};
    }}
"""

APPLY_STYLE_DIRTY = f"""
    QPushButton#applyBtn {{
        background-color: #c0404a;
        border: 1px solid {RED};
        border-radius: 8px;
        color: white;
        font-size: 14px;
        font-weight: 600;
        padding: 10px 32px;
        min-height: 20px;
    }}
    QPushButton#applyBtn:hover {{
        background-color: {RED};
    }}
    QPushButton#applyBtn:disabled {{
        background-color: {DARK_SURFACE};
        border-color: {DARK_SURFACE2};
        color: {TEXT_MUTED};
    }}
"""


def _build_tooltip(sig):
    return (
        f"{sig['desc']}\n"
        f"\n"
        f"Formula:  {sig['detail']}\n"
        f"Range:    {sig['range']}\n"
        f"Typical:  {sig['typical']}\n"
        f"\n"
        f"Recommended:  {sig['recommend']}"
    )


def _is_root():
    return os.geteuid() == 0


class SignalListWidget(QListWidget):
    """Draggable signal source list."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setDragEnabled(True)
        self.setDragDropMode(QAbstractItemView.DragOnly)
        self.setDefaultDropAction(Qt.CopyAction)
        self.setSelectionMode(QAbstractItemView.SingleSelection)
        self.setIconSize(QSize(12, 12))
        self.setSpacing(2)

    def startDrag(self, actions):
        item = self.currentItem()
        if not item:
            return
        drag = QDrag(self)
        mime = QMimeData()
        idx = item.data(Qt.UserRole)
        mime.setText(str(idx))
        drag.setMimeData(mime)

        sig = SIGNALS[idx]
        pixmap = QPixmap(220, 36)
        pixmap.fill(Qt.transparent)
        p = QPainter(pixmap)
        p.setRenderHint(QPainter.Antialiasing)
        p.setBrush(QBrush(QColor(sig["color"])))
        p.setPen(QPen(QColor(sig["color"]).darker(130), 1))
        p.drawRoundedRect(0, 0, 219, 35, 8, 8)
        p.setPen(QColor("white"))
        p.setFont(QFont("Inter", 11, QFont.Bold))
        p.drawText(10, 24, f"sig{idx}: {sig['name']}")
        p.end()
        drag.setPixmap(pixmap)

        drag.exec_(Qt.CopyAction)


class SlotDropZone(QFrame):
    """A single configuration slot that accepts signal drops."""

    signalChanged = pyqtSignal()

    def __init__(self, slot_index, parent=None):
        super().__init__(parent)
        self.slot_index = slot_index
        self.signal_index = -1
        self.weight = 0
        self.setAcceptDrops(True)
        self.setMinimumHeight(120)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(8)
        layout.setContentsMargins(14, 14, 14, 14)

        self.setStyleSheet(f"""
            SlotDropZone {{
                background-color: {DARK_SURFACE2};
                border: 2px dashed {DARK_BORDER};
                border-radius: 12px;
            }}
        """)

        # Header
        header = QHBoxLayout()
        slot_label = QLabel(f"Slot {self.slot_index}")
        slot_label.setFont(QFont("Inter", 12, QFont.Bold))
        slot_label.setStyleSheet(f"color: {TEXT_MUTED};")
        header.addWidget(slot_label)
        header.addStretch()

        self.clear_btn = QPushButton("×")
        self.clear_btn.setFixedSize(26, 26)
        self.clear_btn.setStyleSheet(f"""
            QPushButton {{
                background: {DARK_BORDER};
                border: none;
                border-radius: 13px;
                color: {TEXT_MUTED};
                font-size: 16px;
                font-weight: bold;
                padding: 0;
            }}
            QPushButton:hover {{
                background: {RED};
                color: white;
            }}
        """)
        self.clear_btn.clicked.connect(self._clear_slot)
        self.clear_btn.setVisible(False)
        header.addWidget(self.clear_btn)
        layout.addLayout(header)

        # Signal badge
        self.signal_label = QLabel("Drop a signal here")
        self.signal_label.setAlignment(Qt.AlignCenter)
        self.signal_label.setFont(QFont("Inter", 11))
        self.signal_label.setStyleSheet(f"""
            QLabel {{
                color: {TEXT_MUTED};
                padding: 10px;
                background: transparent;
            }}
        """)
        layout.addWidget(self.signal_label)

        # Weight slider row
        slider_row = QHBoxLayout()
        slider_row.setSpacing(10)
        self.weight_label_neg = QLabel("−7")
        self.weight_label_neg.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 11px;")
        slider_row.addWidget(self.weight_label_neg)

        self.slider = QSlider(Qt.Horizontal)
        self.slider.setRange(-7, 7)
        self.slider.setValue(0)
        self.slider.setTickPosition(QSlider.TicksBelow)
        self.slider.setTickInterval(1)
        self.slider.setSingleStep(1)
        self.slider.setEnabled(False)
        self.slider.valueChanged.connect(self._on_weight_change)
        slider_row.addWidget(self.slider, 1)

        self.weight_label_pos = QLabel("+7")
        self.weight_label_pos.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 11px;")
        slider_row.addWidget(self.weight_label_pos)

        self.weight_display = QLabel("0")
        self.weight_display.setFixedWidth(40)
        self.weight_display.setAlignment(Qt.AlignCenter)
        self.weight_display.setFont(QFont("Inter", 14, QFont.Bold))
        self.weight_display.setStyleSheet(f"color: {TEXT_MUTED};")
        slider_row.addWidget(self.weight_display)

        layout.addLayout(slider_row)

    def _clear_slot(self):
        self.signal_index = -1
        self.weight = 0
        self.slider.setValue(0)
        self.slider.setEnabled(False)
        self.clear_btn.setVisible(False)
        self.signal_label.setText("Drop a signal here")
        self.signal_label.setStyleSheet(f"color: {TEXT_MUTED}; padding: 10px; background: transparent;")
        self.weight_display.setText("0")
        self.weight_display.setStyleSheet(f"color: {TEXT_MUTED};")
        self.setStyleSheet(f"""
            SlotDropZone {{
                background-color: {DARK_SURFACE2};
                border: 2px dashed {DARK_BORDER};
                border-radius: 12px;
            }}
        """)
        self.signalChanged.emit()

    def _on_weight_change(self, value):
        self.weight = value
        sign = "+" if value > 0 else ""
        self.weight_display.setText(f"{sign}{value}" if value != 0 else "0")
        if value > 0:
            self.weight_display.setStyleSheet(f"color: {GREEN}; font-weight: bold;")
        elif value < 0:
            self.weight_display.setStyleSheet(f"color: {RED}; font-weight: bold;")
        else:
            self.weight_display.setStyleSheet(f"color: {TEXT_MUTED};")
        self.signalChanged.emit()

    def set_signal(self, sig_idx, weight=None):
        sig = SIGNALS[sig_idx]
        self.signal_index = sig_idx
        self.slider.setEnabled(True)
        self.clear_btn.setVisible(True)
        if weight is not None:
            self.slider.setValue(weight)
            self.weight = weight
        elif self.weight == 0:
            default_w = 1 if sig["sign"] == "+" else -1
            self.slider.setValue(default_w)
            self.weight = default_w

        color = sig["color"]
        self.signal_label.setText(f"sig{sig_idx}: {sig['name']}")
        self.signal_label.setFont(QFont("Inter", 12, QFont.Bold))
        self.signal_label.setStyleSheet(f"""
            QLabel {{
                color: white;
                background-color: {color};
                border-radius: 8px;
                padding: 8px 14px;
            }}
        """)
        self.setStyleSheet(f"""
            SlotDropZone {{
                background-color: {DARK_SURFACE2};
                border: 2px solid {color};
                border-radius: 12px;
            }}
        """)
        self.signalChanged.emit()

    def dragEnterEvent(self, event):
        if event.mimeData().hasText():
            event.acceptProposedAction()
            self.setStyleSheet(f"""
                SlotDropZone {{
                    background-color: #3a3a55;
                    border: 2px solid {ACCENT};
                    border-radius: 12px;
                }}
            """)

    def dragLeaveEvent(self, event):
        if self.signal_index >= 0:
            color = SIGNALS[self.signal_index]["color"]
            self.setStyleSheet(f"""
                SlotDropZone {{
                    background-color: {DARK_SURFACE2};
                    border: 2px solid {color};
                    border-radius: 12px;
                }}
            """)
        else:
            self.setStyleSheet(f"""
                SlotDropZone {{
                    background-color: {DARK_SURFACE2};
                    border: 2px dashed {DARK_BORDER};
                    border-radius: 12px;
                }}
            """)

    def dropEvent(self, event):
        try:
            sig_idx = int(event.mimeData().text())
        except (ValueError, TypeError):
            return
        if 0 <= sig_idx <= 7:
            self.set_signal(sig_idx)
            event.acceptProposedAction()


class ToggleSwitch(QWidget):
    """Custom animated toggle switch."""

    toggled = pyqtSignal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._checked = False
        self._interactive = True
        self.setFixedSize(52, 28)
        self.setCursor(Qt.PointingHandCursor)
        self._circle_x = 4.0
        self._anim = QPropertyAnimation(self, b"circle_x")
        self._anim.setDuration(200)
        self._anim.setEasingCurve(QEasingCurve.InOutCubic)

    def setInteractive(self, val):
        self._interactive = val
        self.setCursor(Qt.PointingHandCursor if val else Qt.ForbiddenCursor)

    def isChecked(self):
        return self._checked

    def setChecked(self, val):
        self._checked = val
        self._anim.stop()
        self._circle_x = 28.0 if val else 4.0
        self.update()

    @pyqtProperty(float)
    def circle_x(self):
        return self._circle_x

    @circle_x.setter
    def circle_x(self, val):
        self._circle_x = val
        self.update()

    def mousePressEvent(self, event):
        if not self._interactive:
            return
        self._checked = not self._checked
        self._anim.setStartValue(self._circle_x)
        self._anim.setEndValue(28.0 if self._checked else 4.0)
        self._anim.start()
        self.toggled.emit(self._checked)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        if not self._interactive:
            # Grayed out but still shows on/off state
            if self._checked:
                p.setBrush(QBrush(QColor(ACCENT_DIM).darker(150)))
                p.setPen(QPen(QColor(ACCENT_DIM), 1))
            else:
                p.setBrush(QBrush(QColor(DARK_BORDER)))
                p.setPen(QPen(QColor(TEXT_MUTED), 1))
            p.drawRoundedRect(0, 0, 52, 28, 14, 14)
            p.setBrush(QBrush(QColor(TEXT_SECONDARY)))
            p.setPen(Qt.NoPen)
            p.drawEllipse(int(self._circle_x), 4, 20, 20)
        else:
            if self._checked:
                p.setBrush(QBrush(QColor(ACCENT_DIM)))
                p.setPen(QPen(QColor(ACCENT), 1))
            else:
                p.setBrush(QBrush(QColor(DARK_BORDER)))
                p.setPen(QPen(QColor(TEXT_MUTED), 1))
            p.drawRoundedRect(0, 0, 52, 28, 14, 14)
            p.setBrush(QBrush(QColor("white")))
            p.setPen(Qt.NoPen)
            p.drawEllipse(int(self._circle_x), 4, 20, 20)
        p.end()


class CambysesConfigWindow(QMainWindow):

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Cambyses Configuration")
        self.setMinimumSize(900, 640)
        self.resize(960, 700)
        self.setStyleSheet(STYLESHEET)
        self._is_root = _is_root()
        self._dirty = False

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(16)
        main_layout.setContentsMargins(24, 20, 24, 20)

        # Title bar
        title_row = QHBoxLayout()
        title = QLabel("Cambyses")
        title.setFont(QFont("Inter", 22, QFont.Bold))
        title.setStyleSheet(f"color: {ACCENT};")
        title_row.addWidget(title)

        subtitle = QLabel("Scheduler Configuration")
        subtitle.setFont(QFont("Inter", 14))
        subtitle.setStyleSheet(f"color: {TEXT_SECONDARY}; padding-top: 6px;")
        title_row.addWidget(subtitle)

        if not self._is_root:
            ro_label = QLabel("(read-only)")
            ro_label.setFont(QFont("Inter", 11))
            ro_label.setStyleSheet(f"color: {RED}; padding-top: 8px; padding-left: 8px;")
            title_row.addWidget(ro_label)

        title_row.addStretch()

        # Enable toggle
        enable_container = QHBoxLayout()
        enable_container.setSpacing(10)
        self.status_label = QLabel("Disabled")
        self.status_label.setFont(QFont("Inter", 12, QFont.Bold))
        self.status_label.setStyleSheet(f"color: {RED};")
        enable_container.addWidget(self.status_label)
        self.toggle = ToggleSwitch()
        self.toggle.toggled.connect(self._on_toggle)
        enable_container.addWidget(self.toggle)
        title_row.addLayout(enable_container)

        main_layout.addLayout(title_row)

        # Body: signals list + slots
        body = QHBoxLayout()
        body.setSpacing(16)

        # Signal source panel
        sig_group = QGroupBox("Signal Sources")
        sig_layout = QVBoxLayout(sig_group)
        sig_layout.setSpacing(4)

        hint = QLabel("Drag signals to slots →")
        hint.setFont(QFont("Inter", 11))
        hint.setStyleSheet(f"color: {TEXT_MUTED};")
        hint.setAlignment(Qt.AlignCenter)
        sig_layout.addWidget(hint)

        self.signal_list = SignalListWidget()
        for sig in SIGNALS:
            item = QListWidgetItem(f"sig{sig['index']}: {sig['name']}")
            item.setData(Qt.UserRole, sig["index"])
            item.setToolTip(_build_tooltip(sig))
            self.signal_list.addItem(item)
        sig_layout.addWidget(self.signal_list)

        sig_group.setFixedWidth(260)
        body.addWidget(sig_group)

        # Slots panel
        slots_group = QGroupBox("Configuration Slots")
        slots_layout = QVBoxLayout(slots_group)
        slots_layout.setSpacing(12)

        self.slots = []
        for i in range(4):
            slot = SlotDropZone(i)
            slot.signalChanged.connect(self._on_ui_changed)
            self.slots.append(slot)
            slots_layout.addWidget(slot)

        body.addWidget(slots_group, 1)
        main_layout.addLayout(body, 1)

        # Footer: preview + buttons
        footer = QHBoxLayout()
        footer.setSpacing(10)

        self.preview_label = QLabel("")
        self.preview_label.setFont(QFont("Fira Code", 12))
        self.preview_label.setStyleSheet(f"""
            QLabel {{
                background-color: {DARK_SURFACE};
                border: 1px solid {DARK_BORDER};
                border-radius: 8px;
                padding: 10px 16px;
                color: {ACCENT};
            }}
        """)
        footer.addWidget(self.preview_label, 1)

        self.copy_btn = QPushButton("Copy")
        self.copy_btn.setToolTip("Copy sysctl command to clipboard")
        self.copy_btn.clicked.connect(self._copy_to_clipboard)
        footer.addWidget(self.copy_btn)

        self.read_btn = QPushButton("Read Current")
        self.read_btn.clicked.connect(self._read_current)
        footer.addWidget(self.read_btn)

        self.reset_btn = QPushButton("Reset Default")
        self.reset_btn.clicked.connect(self._reset_default)
        footer.addWidget(self.reset_btn)

        self.apply_btn = QPushButton("Apply")
        self.apply_btn.setObjectName("applyBtn")
        self.apply_btn.clicked.connect(self._apply)
        footer.addWidget(self.apply_btn)

        main_layout.addLayout(footer)

        self._set_apply_style_clean()
        self._update_preview()
        self._read_current()

        # Apply root-based restrictions after initial read
        if not self._is_root:
            self._set_readonly_mode()

    def _set_readonly_mode(self):
        """Disable all editing controls when not root."""
        self.toggle.setInteractive(False)
        self.signal_list.setDragEnabled(False)
        for slot in self.slots:
            slot.setAcceptDrops(False)
            slot.slider.setEnabled(False)
            slot.clear_btn.setEnabled(False)
        self.reset_btn.setEnabled(False)
        self.apply_btn.setEnabled(False)

    def _set_apply_style_clean(self):
        self.apply_btn.setStyleSheet(APPLY_STYLE_CLEAN)
        self._dirty = False

    def _set_apply_style_dirty(self):
        if not self._dirty:
            self.apply_btn.setStyleSheet(APPLY_STYLE_DIRTY)
            self._dirty = True

    def _on_ui_changed(self):
        self._update_preview()
        self._set_apply_style_dirty()

    def _on_toggle(self, checked):
        if checked:
            self.status_label.setText("Enabled")
            self.status_label.setStyleSheet(f"color: {GREEN};")
        else:
            self.status_label.setText("Disabled")
            self.status_label.setStyleSheet(f"color: {RED};")
        self._set_apply_style_dirty()

    def _build_config_string(self):
        parts = []
        for slot in self.slots:
            src = slot.signal_index if slot.signal_index >= 0 else 0
            w = slot.weight if slot.signal_index >= 0 else 0
            parts.append(f"{src} {w}")
        return " ".join(parts)

    def _update_preview(self):
        config_str = self._build_config_string()
        self.preview_label.setText(f"kernel.sched_cambyses_config = {config_str}")

    def _copy_to_clipboard(self):
        clipboard = QApplication.clipboard()
        config_str = self._build_config_string()
        enabled = "1" if self.toggle.isChecked() else "0"
        text = (
            f"sysctl kernel.sched_cambyses={enabled}\n"
            f"sysctl kernel.sched_cambyses_config='{config_str}'"
        )
        clipboard.setText(text)

    def _read_current(self):
        """Read current values from sysctl."""
        try:
            with open(SYSCTL_ENABLED, "r") as f:
                enabled = int(f.read().strip())
            self.toggle.setChecked(bool(enabled))
            self._on_toggle(bool(enabled))
        except (FileNotFoundError, PermissionError, ValueError):
            pass

        try:
            with open(SYSCTL_CONFIG, "r") as f:
                vals = list(map(int, f.read().strip().split()))
            if len(vals) == 8:
                for i, slot in enumerate(self.slots):
                    src = vals[i * 2]
                    w = vals[i * 2 + 1]
                    if 0 <= src <= 7:
                        slot.set_signal(src, w)
                    else:
                        slot._clear_slot()
        except (FileNotFoundError, PermissionError, ValueError):
            pass

        self._update_preview()
        self._set_apply_style_clean()

    def _reset_default(self):
        defaults = [(0, 2), (1, 1), (2, 1), (3, -3)]
        for slot, (src, w) in zip(self.slots, defaults):
            slot.set_signal(src, w)
        self.toggle.setChecked(True)
        self._on_toggle(True)
        self._update_preview()
        self._set_apply_style_dirty()

    def _apply(self):
        config_str = self._build_config_string()
        enabled = "1" if self.toggle.isChecked() else "0"

        errors = []
        try:
            with open(SYSCTL_ENABLED, "w") as f:
                f.write(enabled)
        except PermissionError:
            errors.append(f"sched_cambyses: Permission denied")
        except FileNotFoundError:
            errors.append(f"sched_cambyses: File not found (kernel module not loaded?)")

        try:
            with open(SYSCTL_CONFIG, "w") as f:
                f.write(config_str)
        except PermissionError:
            errors.append(f"sched_cambyses_config: Permission denied")
        except FileNotFoundError:
            errors.append(f"sched_cambyses_config: File not found")

        if errors:
            QMessageBox.warning(self, "Error", "Failed to apply:\n" + "\n".join(errors))
            # Keep dirty (red) style
        else:
            self._set_apply_style_clean()


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    palette = QPalette()
    palette.setColor(QPalette.Window, QColor(DARK_BG))
    palette.setColor(QPalette.WindowText, QColor(TEXT_PRIMARY))
    palette.setColor(QPalette.Base, QColor(DARK_SURFACE))
    palette.setColor(QPalette.AlternateBase, QColor(DARK_SURFACE2))
    palette.setColor(QPalette.ToolTipBase, QColor(DARK_SURFACE2))
    palette.setColor(QPalette.ToolTipText, QColor(TEXT_PRIMARY))
    palette.setColor(QPalette.Text, QColor(TEXT_PRIMARY))
    palette.setColor(QPalette.Button, QColor(DARK_SURFACE2))
    palette.setColor(QPalette.ButtonText, QColor(TEXT_PRIMARY))
    palette.setColor(QPalette.Highlight, QColor(ACCENT))
    palette.setColor(QPalette.HighlightedText, QColor("white"))
    app.setPalette(palette)

    window = CambysesConfigWindow()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
