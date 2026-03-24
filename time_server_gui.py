# -*- coding: utf-8 -*-
"""
stm32_timesync_gui.py  —  PC端时间同步脚本（PyQt5图形界面）
用法: python stm32_timesync_gui.py

协议与功能同原命令行版本，仅增加图形界面。
"""

import sys
import serial
import serial.tools.list_ports
import time
from datetime import datetime

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QComboBox, QPushButton, QTextEdit, QLabel, QMessageBox, QGroupBox
)
from PyQt5.QtCore import QThread, pyqtSignal, Qt


# ── 配置（与原脚本一致）──────────────────────────────────────────
BAUD         = 115200
ACK_TIMEOUT  = 5.0
MAX_RETRIES  = 100
# ─────────────────────────────────────────────────────────────


def get_available_ports():
    """返回可用串口列表，每个元素为 (端口名, 描述)"""
    ports = []
    for port in serial.tools.list_ports.comports():
        ports.append((port.device, port.description))
    return ports


def build_frame(dt: datetime) -> bytes:
    """构造带 XOR 校验的时间帧（与原脚本相同）"""
    payload = dt.strftime("TIME,%Y-%m-%d %H:%M:%S")
    xor = 0
    for ch in payload:
        xor ^= ord(ch)
    return b'@' + payload.encode("ascii") + b'#' + bytes([xor])


def wait_for_response(ser: serial.Serial, timeout: float) -> str:
    """等待一行应答（与原脚本相同）"""
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(16)
        if chunk:
            buf += chunk
            if b'\n' in buf:
                line = buf.split(b'\n')[0].strip()
                return line.decode("ascii", errors="replace")
        else:
            time.sleep(0.01)
    return ""


# ── 工作线程 ─────────────────────────────────────────────────
class SyncWorker(QThread):
    """在后台线程中执行同步任务，通过信号将日志和结果传递给主界面"""
    log_signal = pyqtSignal(str)      # 传递日志信息
    finished_signal = pyqtSignal(bool)  # 传递最终结果（成功/失败）

    def __init__(self, port):
        super().__init__()
        self.port = port

    def log(self, msg):
        """发送日志到主线程"""
        self.log_signal.emit(msg)

    def run(self):
        """线程入口：执行同步过程"""
        success = self.sync_time(self.port)
        self.finished_signal.emit(success)

    def sync_time(self, port: str) -> bool:
        """同步时间逻辑（与原脚本相同，但用 self.log 替代 print）"""
        try:
            ser = serial.Serial(port, BAUD, timeout=0.1)
        except serial.SerialException as e:
            self.log(f"[ERROR] 无法打开串口 {port}: {e}")
            return False

        self.log(f"[INFO] 串口已打开: {port} @ {BAUD} baud")
        self.log("[INFO] 等待 STM32 就绪（最长 35 秒）...")

        for attempt in range(1, MAX_RETRIES + 1):
            now = datetime.now()
            frame = build_frame(now)

            # 显示发送信息（帧内容，校验值，时间）
            frame_str = frame[:-1].decode('ascii', errors='replace')
            self.log(f"[发送 #{attempt}] {frame_str}  校验=0x{frame[-1]:02X}  时间={now.strftime('%H:%M:%S')}")

            ser.reset_input_buffer()
            ser.write(frame)

            response = wait_for_response(ser, ACK_TIMEOUT)

            if response == "ACK":
                self.log("[OK] STM32 已确认，时间同步成功。")
                ser.close()
                return True
            elif response == "NAK":
                self.log(f"[WARN] 收到 NAK（校验或内容错误），准备重传...")
            else:
                self.log(f"[WARN] 超时未收到应答（收到: {repr(response)}），准备重传...")

            time.sleep(0.5)

        self.log(f"[ERROR] 重试 {MAX_RETRIES} 次后仍未成功。")
        ser.close()
        return False


# ── 主窗口 ───────────────────────────────────────────────────
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("STM32时间校准")
        self.setMinimumSize(600, 400)

        # 中央部件
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        layout = QVBoxLayout(central_widget)

        # ── 控制区域 ─────────────────────────────────────────
        control_group = QGroupBox("设置")
        control_layout = QHBoxLayout()

        # 串口选择
        control_layout.addWidget(QLabel("串口:"))
        self.combo_port = QComboBox()
        self.combo_port.setMinimumWidth(150)
        control_layout.addWidget(self.combo_port)

        # 刷新按钮
        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.clicked.connect(self.refresh_ports)
        control_layout.addWidget(self.btn_refresh)

        # 波特率显示（只读）
        control_layout.addWidget(QLabel("波特率:"))
        lbl_baud = QLabel(str(BAUD))
        control_layout.addWidget(lbl_baud)

        control_layout.addStretch()
        control_group.setLayout(control_layout)
        layout.addWidget(control_group)

        # ── 操作区域 ─────────────────────────────────────────
        action_group = QGroupBox("操作")
        action_layout = QHBoxLayout()
        self.btn_sync = QPushButton("同步时间")
        self.btn_sync.clicked.connect(self.start_sync)
        self.btn_sync.setMinimumHeight(40)
        action_layout.addWidget(self.btn_sync)
        action_group.setLayout(action_layout)
        layout.addWidget(action_group)

        # ── 日志区域 ─────────────────────────────────────────
        log_group = QGroupBox("日志")
        log_layout = QVBoxLayout()
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        log_layout.addWidget(self.log_text)
        log_group.setLayout(log_layout)
        layout.addWidget(log_group)

        # 初始化串口列表
        self.refresh_ports()

        # 工作线程引用（避免被垃圾回收）
        self.worker = None

    def refresh_ports(self):
        """扫描并刷新串口下拉列表"""
        self.combo_port.clear()
        ports = get_available_ports()
        if ports:
            for port, desc in ports:
                self.combo_port.addItem(f"{port} - {desc}", port)
        else:
            self.combo_port.addItem("无可用串口", "")
            self.combo_port.setEnabled(False)
            self.btn_sync.setEnabled(False)

    def start_sync(self):
        """开始同步时间（启动后台线程）"""
        if self.worker and self.worker.isRunning():
            QMessageBox.warning(self, "提示", "同步任务正在进行中，请稍后...")
            return

        port = self.combo_port.currentData()
        if not port:
            QMessageBox.warning(self, "错误", "请选择一个有效的串口")
            return

        # 清空日志
        self.log_text.clear()

        # 禁用控件，防止重复操作
        self.btn_sync.setEnabled(False)
        self.btn_refresh.setEnabled(False)
        self.combo_port.setEnabled(False)

        # 创建并启动工作线程
        self.worker = SyncWorker(port)
        self.worker.log_signal.connect(self.append_log)
        self.worker.finished_signal.connect(self.on_sync_finished)
        self.worker.start()

    def append_log(self, msg):
        """将日志追加到文本框，并自动滚动到底部"""
        self.log_text.append(msg)
        # 滚动到底部
        cursor = self.log_text.textCursor()
        cursor.movePosition(cursor.End)
        self.log_text.setTextCursor(cursor)

    def on_sync_finished(self, success):
        """同步结束，恢复界面"""
        self.btn_sync.setEnabled(True)
        self.btn_refresh.setEnabled(True)
        self.combo_port.setEnabled(True)

        if success:
            QMessageBox.information(self, "完成", "时间同步成功！")
        else:
            QMessageBox.critical(self, "失败", "时间同步失败，请检查串口连接和STM32程序。")

        self.worker = None


# ── 程序入口 ─────────────────────────────────────────────────
def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()