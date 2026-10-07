"""设备级 Vendor 控制请求转发探针（2026-09-27）。

为什么需要它：Windows 用户态**无法**向 HID 类设备发任意 vendor 控制请求——HID API
只支持 HID 类请求，要发 vendor 得把驱动换成 WinUSB/libusb，那会破坏 G HUB。
所以这里借用板子已有的 UART0 诊断注入通道，把一条 VENDOR_CONTROL_REQUEST 直接投给
M 侧的控制帧处理，验证「解码 → 控制队列 → 通用 EP0 URB → 物理设备」这条链路。

它验证什么、不验证什么：
  验证：M 侧收到 vendor 请求后能否用通用 EP0 通道发给物理设备，以及设备的状态回应。
  不验证：P 侧 TinyUSB 回调（tud_vendor_control_xfer_cb）与板间 UART1 转发——
          那一段需要主机真的发一笔 vendor 请求才能触发，本环境做不到。

用法：
    python tools/vendor_control_probe.py --port COM13
    python tools/vendor_control_probe.py --port COM13 --bm 0xC0 --request 0x00 --length 8
    python tools/vendor_control_probe.py --port COM13 --bm 0x40 --request 0x09 --value 0x0310 --out-hex 0102

判定（看 M 侧回显的日志行）：
    「Vendor 控制请求：bm=.. result=ESP_OK」      → 通道打通，设备受理了这笔请求
    「Vendor 控制请求：bm=.. result=ESP_FAIL」    → 通道打通，设备 STALL（链路同样已通）
    「Vendor 控制请求：bm=.. result=ESP_ERR_TIMEOUT」 → 通用 EP0 通道可用但设备未响应
    完全没有该日志 → 注入被拒（参数不合法）或板子不在该串口
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serial  # noqa: E402

from uart_protocol import build_frame  # noqa: E402

# Windows 控制台默认 GBK：串口日志里可能混入非 UTF-8 字节（多任务日志交错），
# 统一改成 UTF-8 容错输出，否则 print 会直接抛 UnicodeEncodeError。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

MESSAGE_DIAG_INJECT_REQUEST = 0x17
MESSAGE_VENDOR_CONTROL_REQUEST = 0x33
ROUTE_M_CONTROL = 2  # 注入路由：2 = M 的物理鼠标控制侧


def parse_hex(text: str) -> bytes:
    cleaned = text.replace(" ", "").replace(",", "").replace("0x", "")
    if not cleaned:
        return b""
    return bytes.fromhex(cleaned)


def main() -> int:
    parser = argparse.ArgumentParser(description="设备级 Vendor 控制请求转发探针")
    parser.add_argument("--port", required=True, help="M 板的 UART0 串口（例如 COM13）")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--bm", type=lambda v: int(v, 0), default=0xC0,
                        help="bmRequestType（默认 0xC0 = 设备→主机、厂商自定义、设备级）")
    parser.add_argument("--request", type=lambda v: int(v, 0), default=0x00, help="bRequest")
    parser.add_argument("--value", type=lambda v: int(v, 0), default=0, help="wValue")
    parser.add_argument("--index", type=lambda v: int(v, 0), default=0, help="wIndex")
    parser.add_argument("--length", type=int, default=8, help="wLength（未给 --out-hex 时使用）")
    parser.add_argument("--out-hex", default="", help="OUT 方向的数据（十六进制，给出时覆盖 --length）")
    parser.add_argument("--watch", type=float, default=4.0, help="发送后监听日志的秒数")
    args = parser.parse_args()

    payload_data = parse_hex(args.out_hex)
    w_length = len(payload_data) if payload_data else args.length
    # 板间 payload：[transaction_id:2][bm:1][req:1][wValue:2][wIndex:2]
    #               [data_length:2][wLength:2][data]
    # data_length 与 wLength 必须分开：IN 请求的 wLength 是"期望读取长度"，不带数据。
    vendor_payload = struct.pack("<HBBHHHH", 0x4321, args.bm & 0xFF, args.request & 0xFF,
                                 args.value & 0xFFFF, args.index & 0xFFFF,
                                 len(payload_data), w_length & 0xFFFF) + payload_data
    inject_payload = bytes([ROUTE_M_CONTROL, MESSAGE_VENDOR_CONTROL_REQUEST]) + vendor_payload

    # 必须「先设 DTR/RTS 再 open」：否则打开瞬间的默认电平会经 CH340 复位板子，
    # 注入就落在启动过程中而被丢掉（本项目 capture_dual_serial.py 的 PortRecorder
    # 同样这么处理）。
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    port.dtr = False
    port.rts = False
    try:
        port.open()
    except serial.SerialException as error:
        print("打开 %s 失败：%s" % (args.port, error))
        return 1
    time.sleep(0.2)
    port.reset_input_buffer()
    port.write(build_frame(MESSAGE_DIAG_INJECT_REQUEST, 1, inject_payload))
    port.flush()
    print("已注入 Vendor 控制请求：route=%d type=0x%02X bm=0x%02X req=0x%02X "
          "value=0x%04X index=0x%04X wLength=%u（数据 %u 字节）"
          % (ROUTE_M_CONTROL, MESSAGE_VENDOR_CONTROL_REQUEST, args.bm, args.request,
             args.value, args.index, w_length, len(payload_data)))

    deadline = time.monotonic() + args.watch
    hits = 0
    while time.monotonic() < deadline:
        line = port.readline()
        if not line:
            continue
        text = line.decode("utf-8", errors="replace").strip()
        if any(token in text for token in ("Vendor 控制请求", "VENDOR_CONTROL", "注入")):
            hits += 1
            print("  " + text)
    port.close()
    if hits == 0:
        print("（%.1f 秒内没有捕获到相关日志：可能注入被拒、板子不在该串口，"
              "或设备侧没有产生日志）" % args.watch)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
