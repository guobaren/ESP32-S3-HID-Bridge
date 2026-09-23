"""对双板 PC 侧原生 USB CDC 做无 EXE 的协议 v2 小幅往返测试。

脚本默认先用 DeviceProbe/DeviceHello 确认 HID Bridge，再发送 SessionStart、+X、-X 和 Ping，
不发送点击、键盘或 SendInput。CDC 只承载二进制协议，UART0/CH340 仅用于刷写和日志。
同一个原生 CDC COM 必须由本脚本独占。
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import secrets
import sys
import time
from pathlib import Path
from typing import Optional


MAGIC = b"\xA5\x5A"
VERSION = 2
TYPE_MOUSE = 0x02
TYPE_PING = 0x04
TYPE_SESSION_START = 0x05
TYPE_DEVICE_PROBE = 0x06
TYPE_DEVICE_HELLO = 0x07
DEVICE_SIGNATURE = b"HIDBRDG2"


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(message_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > 64:
        raise ValueError("payload too long")
    body = bytes((VERSION, message_type, sequence & 0xFF, (sequence >> 8) & 0xFF, len(payload))) + payload
    return MAGIC + body + crc16_ccitt(body).to_bytes(2, "little")


class SerialTap:
    def __init__(self, log_path: Optional[Path], expected_probe_nonce: Optional[bytes]) -> None:
        self.buffer = bytearray()
        self.text_buffer = bytearray()
        self.frame_count = 0
        self.invalid_count = 0
        self.text_line_count = 0
        self.device_hello = False
        self.device_role: Optional[int] = None
        self.expected_probe_nonce = expected_probe_nonce
        self.log_file = log_path.open("ab") if log_path else None

    def close(self) -> None:
        if self.log_file:
            self.log_file.close()

    def feed(self, data: bytes) -> None:
        if not data:
            return
        if self.log_file:
            self.log_file.write(data)
            self.log_file.flush()
        self.buffer.extend(data)
        self.text_buffer.extend(data)
        self._print_text_lines()
        self._parse_frames()

    def _print_text_lines(self) -> None:
        while b"\n" in self.text_buffer:
            line, _, rest = self.text_buffer.partition(b"\n")
            self.text_buffer = bytearray(rest)
            clean = bytes(ch for ch in line if ch in (9, 13) or 32 <= ch <= 126)
            if clean and len(clean) * 100 >= max(1, len(line)) * 70:
                self.text_line_count += 1
                print(f"[device] {clean.decode('ascii', errors='replace')}")

    def _parse_frames(self) -> None:
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                # Keep a possible first magic byte for the next read.
                self.buffer = self.buffer[-1:] if self.buffer[-1:] == MAGIC[:1] else bytearray()
                return
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 7:
                return
            payload_length = self.buffer[6]
            if payload_length > 64:
                self.invalid_count += 1
                del self.buffer[:1]
                continue
            total = 9 + payload_length
            if len(self.buffer) < total:
                return
            candidate = bytes(self.buffer[:total])
            del self.buffer[:total]
            body = candidate[2:-2]
            expected = int.from_bytes(candidate[-2:], "little")
            if candidate[2] != VERSION or crc16_ccitt(body) != expected:
                self.invalid_count += 1
                continue
            self.frame_count += 1
            message_type = candidate[3]
            sequence = int.from_bytes(candidate[4:6], "little")
            payload = candidate[7:7 + payload_length]
            if (
                message_type == TYPE_DEVICE_HELLO
                and payload[:8] == DEVICE_SIGNATURE
                and self.expected_probe_nonce is not None
                and payload[8:16] == self.expected_probe_nonce
                and len(payload) in (16, 17)
            ):
                self.device_hello = True
                self.device_role = payload[16] if len(payload) == 17 else None
                role_name = {1: "PC_DEVICE", 2: "MOUSE_HOST"}.get(
                    self.device_role, "legacy/unknown")
                print(f"[device] DeviceHello HIDBRDG2 nonce matched role={role_name}")
            print(f"[frame] type=0x{message_type:02X} seq={sequence} payload={payload_length}")


def cursor_position() -> Optional[tuple[int, int]]:
    if sys.platform != "win32":
        return None
    class Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    point = Point()
    if ctypes.windll.user32.GetCursorPos(ctypes.byref(point)):
        return point.x, point.y
    return None


def read_for(port, tap: SerialTap, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            tap.feed(chunk)


def main() -> int:
    parser = argparse.ArgumentParser(description="双板 HID Proxy 鼠标侧 UART0 协议 v2 小幅往返测试")
    parser.add_argument("--port", required=True, help="鼠标侧板 CH340/UART0 COM 端口，例如 COM7")
    parser.add_argument("--baud", type=int, default=921600, help="CDC兼容参数；协议不依赖UART波特率")
    parser.add_argument("--step", type=int, default=4, help="X 方向测试步长，默认 4")
    parser.add_argument("--pause", type=float, default=0.25, help="+X 与 -X 之间的等待秒数")
    parser.add_argument("--read-seconds", type=float, default=1.0, help="完成后继续读取设备输出的秒数")
    parser.add_argument("--cursor", action="store_true", help="只读取 Windows 光标位置，不发送本机输入")
    parser.add_argument("--log", type=Path, help="保存 UART 原始字节的文件路径")
    parser.add_argument("--no-probe", action="store_true", help="跳过 DeviceProbe（不推荐，可能选错 COM）")
    parser.add_argument("--probe-only", action="store_true", help="只确认设备角色，不发送会话或移动命令")
    args = parser.parse_args()

    if args.step == 0 or not -32768 <= args.step <= 32767:
        parser.error("--step必须非0且在int16范围内（-32768..32767）")
    if args.pause < 0:
        parser.error("--pause不能为负数")
    if args.read_seconds < 0:
        parser.error("--read-seconds不能为负数")

    try:
        import serial  # type: ignore
    except ImportError:
        print("缺少 pyserial：请在当前 Python 环境安装 pyserial", file=sys.stderr)
        return 2

    log_path = args.log
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    probe_nonce = secrets.token_bytes(8)
    tap = SerialTap(log_path, None if args.no_probe else probe_nonce)
    before = cursor_position() if args.cursor else None
    after_positive = None
    after_return = None
    port = None
    sequence = 0
    try:
        port = serial.Serial(
            port=None,
            baudrate=args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.05,
            write_timeout=1.0,
            rtscts=False,
            dsrdtr=False,
        )
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        port.reset_input_buffer()

        def send(message_type: int, payload: bytes = b"", read_seconds: float = 0.10) -> None:
            nonlocal sequence
            frame = encode_frame(message_type, sequence, payload)
            written = port.write(frame)
            if written != len(frame):
                raise OSError(f"CDC写入不完整：{written}/{len(frame)}")
            port.flush()
            print(f"[send] type=0x{message_type:02X} seq={sequence} bytes={len(frame)}")
            sequence = (sequence + 1) & 0xFFFF
            read_for(port, tap, read_seconds)

        if not args.no_probe:
            send(TYPE_DEVICE_PROBE, probe_nonce, read_seconds=0.25)
            if not tap.device_hello:
                print("未收到匹配的 DeviceHello/HIDBRDG2，停止发送移动命令。", file=sys.stderr)
                return 1
            if tap.device_role not in (None, 2):
                print("目标串口不是MOUSE_HOST，停止发送移动命令。", file=sys.stderr)
                return 1
        if args.probe_only:
            print("[summary] probe_only=true role=MOUSE_HOST；未发送会话或移动命令。")
            return 0
        send(TYPE_SESSION_START)
        send(TYPE_MOUSE, bytes((0, args.step & 0xFF, (args.step >> 8) & 0xFF, 0, 0, 0, 0, 0)))
        time.sleep(args.pause)
        after_positive = cursor_position() if args.cursor else None
        if args.cursor:
            print(f"[cursor] cursor_after_positive={after_positive}")
        send(TYPE_MOUSE, bytes((0, (-args.step) & 0xFF, ((-args.step) >> 8) & 0xFF, 0, 0, 0, 0, 0)))
        send(TYPE_PING)
        read_for(port, tap, args.read_seconds)
    except OSError as error:
        print(f"无法独占打开 {args.port}: {error}", file=sys.stderr)
        return 1
    finally:
        if port is not None and port.is_open:
            port.close()
        tap.close()

    after_return = cursor_position() if args.cursor else None
    print(
        "[summary] valid_frames={} invalid_candidates={} text_lines={} before={} "
        "after_positive={} after_return={}".format(
            tap.frame_count,
            tap.invalid_count,
            tap.text_line_count,
            before,
            after_positive,
            after_return,
        )
    )
    if args.cursor:
        positive_changed = before is not None and after_positive is not None and before != after_positive
        print(
            "[summary] positive_move_observed={}；只要 before 与 after_positive 中间位置变化，"
            "即可证明正向命令生效；after_return 仅作回位辅助观察。".format(positive_changed)
        )
    print("[summary] 未发送点击、键盘或 SendInput；光标变化（若启用）只作为观测，不作为固件协议通过依据。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
