"""向 M 板注入已知位移的鼠标报告——走与物理鼠标**完全相同**的路径。

为什么需要它：物理鼠标由手部移动驱动，位移不可控，因此无法判断 M/P 两侧的位移统计
究竟哪一侧不可信。本工具把一段原始鼠标报告直接投进 M 的物理 RX 回调
（`dual_hid_host_inject_report`），于是位移统计、入队、板间转发、P 侧提交全都与真实
报告走**同一条路**，唯一区别是位移**完全已知**——可据此做受控实验：

    注入 N 帧 × 每帧 dx=10 → 预期 M `motion_rx_dx` = +10N、P `motion_fwd_dx` = +10N

报告正文的 8 字节布局（由真实样本反推，与固件布局一致）：
    [0] 按键位  [1] 保留  [2:3] dx(int16 LE)  [4:5] dy(int16 LE)  [6] 滚轮  [7] pan

用法：
    python tools/inject_mouse_motion.py --port COM13 --count 200 --dx 10 --dy 0
    python tools/inject_mouse_motion.py --port COM13 --count 100 --dy -5 --interval-ms 5
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serial  # noqa: E402

from fetch_onboard_log import build_frame  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

# M 侧诊断命令：payload 就是原始报告字节，由 M 投进物理 RX 回调。
MESSAGE_DIAG_REPORT_INJECT_REQUEST = 0x1B


def build_report(dx: int, dy: int, buttons: int = 0, wheel: int = 0, pan: int = 0) -> bytes:
    """按真实布局拼一份 8 字节鼠标报告。"""
    report = struct.pack(
        "<BBhhBB",
        buttons & 0xFF,
        0,
        dx,
        dy,
        wheel & 0xFF,
        pan & 0xFF,
    )
    if len(report) != 8:
        raise RuntimeError("报告长度不是 8 字节：%d" % len(report))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="向 M 板注入已知位移的鼠标报告")
    parser.add_argument("--port", required=True, help="M 板的 UART0 串口（例如 COM13）")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--count", type=int, default=200, help="注入帧数")
    parser.add_argument("--dx", type=int, default=10, help="每帧 dx")
    parser.add_argument("--dy", type=int, default=0, help="每帧 dy")
    parser.add_argument("--interval-ms", type=float, default=2.0,
                        help="帧间隔（默认 2 ms，避免把 UART0 一次灌满）")
    args = parser.parse_args()

    # 固件会根据当前枚举到的鼠标布局自动补上 Report ID，避免工具硬编码 ID。
    report = build_report(args.dx, args.dy)

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    port.dtr = False          # 先设 DTR/RTS 再 open，避免经 CH340 复位板子
    port.rts = False
    try:
        port.open()
    except serial.SerialException as error:
        print("打开 %s 失败：%s" % (args.port, error))
        return 1
    time.sleep(0.2)

    sequence = 1
    for _ in range(args.count):
        port.write(build_frame(MESSAGE_DIAG_REPORT_INJECT_REQUEST, sequence, report))
        sequence = (sequence + 1) & 0xFFFF
        if args.interval_ms > 0:
            time.sleep(args.interval_ms / 1000.0)
    port.flush()
    time.sleep(0.3)
    port.close()

    print("已向 M 注入 %d 帧（报告=%s）：预期两侧位移均为 dx=%d dy=%d"
          % (args.count, report.hex(" "), args.count * args.dx, args.count * args.dy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
