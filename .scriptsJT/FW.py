# ========================================================================
#  ESPixelStick Firmware Updater - "THE HOT SHIT EDITION" + PER-FW SELECT
# ========================================================================

import sys
import os
import time
import requests
import xml.etree.ElementTree as ET

from pathlib import Path
from requests_toolbelt.multipart.encoder import MultipartEncoder

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QFileDialog, QLabel,
    QPushButton, QScrollArea, QFrame, QProgressBar, QTextEdit, QCheckBox,
    QComboBox
)
from PySide6.QtGui import QPalette, QColor, QTextCursor

# ------------------------------------------------------------
# DARK THEME
# ------------------------------------------------------------
def enable_dark_theme(app):
    palette = QPalette()
    palette.setColor(QPalette.Window, QColor(30, 30, 30))
    palette.setColor(QPalette.WindowText, QColor(240, 240, 240))
    palette.setColor(QPalette.Base, QColor(22, 22, 22))
    palette.setColor(QPalette.AlternateBase, QColor(44, 44, 44))
    palette.setColor(QPalette.Button, QColor(40, 40, 40))
    palette.setColor(QPalette.ButtonText, QColor(240, 240, 240))
    palette.setColor(QPalette.Text, QColor(240, 240, 240))
    palette.setColor(QPalette.Highlight, QColor(60, 120, 255))
    palette.setColor(QPalette.HighlightedText, QColor(255, 255, 255))
    app.setPalette(palette)

# ------------------------------------------------------------
# Worker Thread
# ------------------------------------------------------------
class ControllerWorker(QThread):
    progress = Signal(str, int)
    log = Signal(str)
    finished = Signal(str, str)

    def __init__(self, ip, name, firmware_path):
        super().__init__()
        self.ip = ip
        self.name = name
        self.firmware_path = firmware_path

    def run(self):
        ip = self.ip
        fw = self.firmware_path

        try:
            self.log.emit(f"[{ip}] Uploading firmware: {os.path.basename(fw)}")
            self.progress.emit(ip, 10)

            encoder = MultipartEncoder(
                fields={'file': (os.path.basename(fw), open(fw, 'rb'), 'application/octet-stream')}
            )

            r = requests.post(
                f"http://{ip}/updatefw",
                data=encoder,
                headers={'Content-Type': encoder.content_type},
                timeout=30
            )
            if r.status_code != 200:
                raise Exception(f"HTTP {r.status_code}")

            self.progress.emit(ip, 35)
            self.log.emit(f"[{ip}] Firmware accepted, rebooting…")

            # WAIT FOR DEVICE
            import time
            start = time.time()
            while time.time() - start < 180:
                try:
                    r = requests.get(f"http://{ip}/conf/input_config.json", timeout=5)
                    if r.status_code == 200:
                        break
                except:
                    pass
                time.sleep(5)

            self.progress.emit(ip, 60)
            self.log.emit(f"[{ip}] Device online. Configuring FPP…")

            # ENABLE FPP
            js = requests.get(f"http://{ip}/conf/input_config.json").json()
            js["input_config"]["channels"]["1"]["type"] = 5
            requests.post(f"http://{ip}/conf/input_config.json", json=js)

            self.progress.emit(ip, 80)
            self.log.emit(f"[{ip}] Updating hostname/AP…")

            cfg = requests.get(f"http://{ip}/conf/config.json").json()
            cfg["system"]["device"]["id"] = self.name
            cfg["system"]["network"]["hostname"] = self.name
            cfg["system"]["network"]["wifi"]["ap_ssid"] = f"{self.name}-AP"
            requests.post(f"http://{ip}/conf/config.json", json=cfg)

            self.progress.emit(ip, 100)
            self.log.emit(f"[{ip}] COMPLETE.")
            self.finished.emit(ip, "SUCCESS")

        except Exception as e:
            self.log.emit(f"[{ip}] ERROR: {e}")
            self.finished.emit(ip, "FAIL")

# ------------------------------------------------------------
# Discovery
# ------------------------------------------------------------
def is_matrix(ctrl):
    for tag in ["Name", "Description", "comment"]:
        val = ctrl.get(tag, "").lower()
        if "fpp" in val:
            return True
    return False

def load_controllers(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()
    ctrls = root.findall(".//Controller")

    out = []
    for c in ctrls:
        ip = c.get("IP")
        if not ip:
            continue
        if is_matrix(c):
            continue
        name = c.get("Name", "") or c.get("Description", "") or "(unnamed)"
        out.append((ip, name))
    return out

# ------------------------------------------------------------
# GUI
# ------------------------------------------------------------
class FirmwareGUI(QWidget):

    def __init__(self):
        super().__init__()

        self.setWindowTitle("ESPixelStick Firmware Uploader — HOT SHIT EDITION + Multi-FW")
        self.resize(1200, 750)

        self.controllers = []
        self.controller_widgets = {}
        self.xml_path = None

        # NEW: firmware library
        self.firmware_library = []  
        self.workers = []

        layout = QVBoxLayout(self)

        # ---------------- TOP CONTROLS ----------------
        top = QHBoxLayout()
        layout.addLayout(top)

        self.btn_xml = QPushButton("Select xLights XML")
        self.btn_xml.clicked.connect(self.pick_xml)
        top.addWidget(self.btn_xml)

        self.btn_fw = QPushButton("Add Firmware (.efu)")
        self.btn_fw.clicked.connect(self.add_firmware)
        top.addWidget(self.btn_fw)

        self.btn_load = QPushButton("Load Controllers")
        self.btn_load.clicked.connect(self.load_controllers_gui)
        top.addWidget(self.btn_load)

        self.btn_start = QPushButton("Start Batch Update")
        self.btn_start.clicked.connect(self.start_update)
        top.addWidget(self.btn_start)

        top.addStretch()

        # ---------------- CONTROLLER SCROLL ----------------
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        layout.addWidget(self.scroll)

        self.ctrl_container = QWidget()
        self.ctrl_layout = QVBoxLayout(self.ctrl_container)
        self.scroll.setWidget(self.ctrl_container)

        # ---------------- LOG WINDOW ----------------
        self.log_box = QTextEdit()
        self.log_box.setReadOnly(True)
        self.log_box.setFixedHeight(200)
        layout.addWidget(self.log_box)

    # ------------------------------------------------------------------

    def append_log(self, msg):
        self.log_box.append(msg)
        cursor = self.log_box.textCursor()
        cursor.movePosition(QTextCursor.End)
        self.log_box.setTextCursor(cursor)

    # ------------------------------------------------------------------

    def pick_xml(self):
        p, _ = QFileDialog.getOpenFileName(self, "Select xLights networks XML", "", "XML (*.xml)")
        if p:
            self.xml_path = p
            self.append_log(f"Selected XML: {p}")

    def add_firmware(self):
        p, _ = QFileDialog.getOpenFileName(self, "Add Firmware", "", "Firmware (*.efu)")
        if p:
            self.firmware_library.append(p)
            self.append_log(f"Added Firmware: {os.path.basename(p)}")

    # ------------------------------------------------------------------

    def load_controllers_gui(self):
        if not self.xml_path or not os.path.isfile(self.xml_path):
            self.append_log("ERROR: No XML selected.")
            return

        self.controllers = load_controllers(self.xml_path)
        self.append_log(f"Loaded {len(self.controllers)} controllers.")

        # Clear old
        for i in reversed(range(self.ctrl_layout.count())):
            w = self.ctrl_layout.itemAt(i).widget()
            if w:
                w.deleteLater()

        self.controller_widgets.clear()

        # Build cards
        for ip, name in self.controllers:
            frame = QFrame()
            frame.setStyleSheet("background:#222; border:1px solid #444; padding:6px;")
            h = QHBoxLayout(frame)

            # Checkbox
            chk = QCheckBox()
            chk.setChecked(True)
            h.addWidget(chk)

            # Label
            lbl = QLabel(f"<b>{ip}</b> — {name}")
            h.addWidget(lbl)

            # DROPDOWN for FW selection
            fw_select = QComboBox()
            fw_select.addItem("Select Firmware…")
            for fw in self.firmware_library:
                fw_select.addItem(os.path.basename(fw), fw)
            h.addWidget(fw_select)

            # Progress bar
            bar = QProgressBar()
            bar.setValue(0)
            bar.setMaximum(100)
            bar.setFixedWidth(200)
            h.addWidget(bar)

            self.ctrl_layout.addWidget(frame)
            self.controller_widgets[ip] = (chk, fw_select, bar)

        self.ctrl_layout.addStretch()

    # ------------------------------------------------------------------

    def start_update(self):
        selected = []

        for ip, name in self.controllers:
            chk, fw_select, bar = self.controller_widgets[ip]

            if chk.isChecked():
                fw_path = fw_select.currentData()

                if fw_path is None:
                    self.append_log(f"[{ip}] ERROR: No firmware selected!")
                    continue

                selected.append((ip, name, fw_path))

        if not selected:
            self.append_log("ERROR: No valid controller + firmware selections.")
            return

        self.append_log("=== STARTING MULTI-FIRMWARE BATCH UPDATE ===")

        for ip, name, fw in selected:
            worker = ControllerWorker(ip, name, fw)
            worker.progress.connect(self.update_progress)
            worker.log.connect(self.append_log)
            worker.finished.connect(self.worker_done)
            worker.start()
            self.workers.append(worker)

    # ------------------------------------------------------------------

    def update_progress(self, ip, value):
        _, _, bar = self.controller_widgets[ip]
        bar.setValue(value)

    def worker_done(self, ip, status):
        self.append_log(f"[{ip}] STATUS: {status}")

# ------------------------------------------------------------
# MAIN
# ------------------------------------------------------------
def main():
    app = QApplication(sys.argv)
    enable_dark_theme(app)

    gui = FirmwareGUI()
    gui.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
