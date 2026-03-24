"""
stm32_timesync.py  —  PC端时间同步脚本
用法: python stm32_timesync.py [端口]  例: python stm32_timesync.py COM3
                                           python stm32_timesync.py /dev/ttyUSB0

协议：
  发送帧: @TIME,YYYY-MM-DD HH:MM:SS#<xor>
    · xor = payload 所有字节的逐位异或（不含 @ # 定界符）
  应答:
    "ACK"  —— STM32 已成功写入 RTC
    "NAK"  —— 校验失败或内容非法，自动重传（最多 MAX_RETRIES 次）
"""

import serial
import sys
import time
from datetime import datetime


# ── 配置 ──────────────────────────────────────────────────────
PORT         = sys.argv[1] if len(sys.argv) > 1 else "COM8"
BAUD         = 115200
ACK_TIMEOUT  = 5.0    # 等待 ACK/NAK 的超时秒数
MAX_RETRIES  = 100      # NAK 后最大重传次数
# ─────────────────────────────────────────────────────────────


def build_frame(dt: datetime) -> bytes:
    """构造带 XOR 校验的时间帧"""
    payload = dt.strftime("TIME,%Y-%m-%d %H:%M:%S")
    xor = 0
    for ch in payload:
        xor ^= ord(ch)
    # 帧 = '@' + payload + '#' + 校验字节（单个原始字节，非ASCII文本）
    return b'@' + payload.encode("ascii") + b'#' + bytes([xor])


def wait_for_response(ser: serial.Serial, timeout: float) -> str:
    """
    等待一行应答（ACK 或 NAK），超时返回空字符串。
    逐字节读取以兼容不同缓冲行为。
    """
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


def sync_time(port: str) -> bool:
    """打开串口，发送时间帧，等待 ACK，必要时重试。返回是否成功。"""
    try:
        ser = serial.Serial(port, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"[ERROR] 无法打开串口 {port}: {e}")
        return False

    # 等待串口稳定（USB-TTL 驱动枚举 + STM32 启动窗口）
    # 保持串口持续打开——STM32 在启动窗口结束后才开始监听
    print(f"[INFO] 串口已打开: {port} @ {BAUD} baud")
    print("[INFO] 等待 STM32 就绪（最长 35 秒）...")

    for attempt in range(1, MAX_RETRIES + 1):
        # 每次重传都取当前时间，避免重传时时间戳已过期
        now   = datetime.now()
        frame = build_frame(now)

        print(f"[发送 #{attempt}] {frame[:-1].decode('ascii', errors='replace')}"
              f"  校验=0x{frame[-1]:02X}  时间={now.strftime('%H:%M:%S')}")

        ser.reset_input_buffer()
        ser.write(frame)

        response = wait_for_response(ser, ACK_TIMEOUT)

        if response == "ACK":
            print(f"[OK] STM32 已确认，时间同步成功。")
            ser.close()
            return True
        elif response == "NAK":
            print(f"[WARN] 收到 NAK（校验或内容错误），准备重传...")
        else:
            print(f"[WARN] 超时未收到应答（收到: {repr(response)}），准备重传...")

        time.sleep(0.5)

    print(f"[ERROR] 重试 {MAX_RETRIES} 次后仍未成功。")
    ser.close()
    return False


if __name__ == "__main__":
    success = sync_time(PORT)
    sys.exit(0 if success else 1)