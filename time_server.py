# time_server.py - PC端时间同步服务器
# 通过USB转TTL向STM32提供时间同步（替代ESP32）
#
# 依赖：pyserial
#   安装：pip install pyserial
#
# 使用方法：
#   python time_server.py           # 自动搜索串口
#   python time_server.py COM3      # 指定串口

import serial
import serial.tools.list_ports
import datetime
import time
import sys

# ========== 配置 ==========
BAUD_RATE    = 115200
TIMEZONE_OFFSET_HOURS = 8      # UTC+8，与STM32保持一致
# =========================

def list_ports():
    """列出所有可用串口"""
    ports = serial.tools.list_ports.comports()
    return [p.device for p in ports]

def select_port():
    """自动或手动选择串口"""
    # 命令行参数指定
    if len(sys.argv) > 1:
        return sys.argv[1]

    ports = list_ports()
    if not ports:
        print("[ERROR] 未找到任何串口，请检查USB转TTL是否已插入。")
        sys.exit(1)

    if len(ports) == 1:
        print(f"[INFO]  自动选择唯一串口：{ports[0]}")
        return ports[0]

    # 多个串口时让用户选择
    print("[INFO]  检测到以下串口：")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p}")
    while True:
        try:
            idx = int(input("请选择串口编号："))
            if 0 <= idx < len(ports):
                return ports[idx]
        except ValueError:
            pass
        print("输入无效，请重试。")

def get_time_str():
    """返回当前本地时间字符串（UTC+8），格式：YYYY-MM-DD HH:MM:SS"""
    now = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=TIMEZONE_OFFSET_HOURS)
    return now.strftime("%Y-%m-%d %H:%M:%S")

def send_framed(ser, msg):
    """将消息包裹在 @ / # 帧中发送，与STM32的USART驱动协议一致"""
    frame = "@" + msg + "#"
    ser.write(frame.encode("ascii"))
    print(f"[SENT]  {frame}")

def main():
    port = select_port()

    print(f"[INFO]  打开串口 {port}，波特率 {BAUD_RATE}")
    try:
        ser = serial.Serial(
            port     = port,
            baudrate = BAUD_RATE,
            bytesize = serial.EIGHTBITS,
            parity   = serial.PARITY_NONE,
            stopbits = serial.STOPBITS_ONE,
            timeout  = 0.1       # 非阻塞读取
        )
    except serial.SerialException as e:
        print(f"[ERROR] 无法打开串口：{e}")
        sys.exit(1)

    print("[INFO]  等待STM32请求（按 Ctrl+C 退出）...")
    print("-" * 40)

    try:
        while True:
            # 读取一个字节
            data = ser.read(1)
            if data == b'T':
                time_str = get_time_str()
                send_framed(ser, "TIME," + time_str)
            elif data:
                # 收到非预期字节，打印供调试
                print(f"[RECV]  未知字节：{data}")

    except KeyboardInterrupt:
        print("\n[INFO]  用户退出。")
    finally:
        ser.close()
        print("[INFO]  串口已关闭。")

if __name__ == "__main__":
    main()