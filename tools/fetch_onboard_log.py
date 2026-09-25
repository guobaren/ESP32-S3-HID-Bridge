"""按协议从任意一块板下载板载滚动日志（不需要 EXE）。

板载日志在每块板的 SPIFFS 分区里，跨复位续写（4×128KB 轮转）。这个脚本在
**任意一块板自己的 UART0** 上直接发主机协议命令取回日志，因此：

* 鼠标侧板：把它的调试 USB-C 接到本机，用 `--port COMx` 取；
* 电脑侧板：同样接它的调试 USB-C（它在这条线上只跑日志服务，不接受输入会话）。

命令：`LOG_READ_REQUEST{offset:u32, max_bytes:u8}` → `LOG_READ_RESPONSE{offset, total, data}`。
打开串口时显式放开 DTR/RTS，避免取日志这个动作本身复位被测板。

示例：
    python tools/fetch_onboard_log.py --port COM3 --out artifacts/tests/onboard-P.log
    python tools/fetch_onboard_log.py --port COM14 --clear-before --out board.log
"""

from __future__ import annotations

import argparse
import datetime
import struct
import sys
import time
from pathlib import Path

import serial

FRAME_MAGIC = b"\xA5\x5A"
PROTOCOL_VERSION = 0x02
MESSAGE_LOG_READ_REQUEST = 0x08
MESSAGE_LOG_READ_RESPONSE = 0x09
MESSAGE_LOG_CLEAR_REQUEST = 0x0A
MESSAGE_LOG_DUMP_REQUEST = 0x0B
MESSAGE_LOG_CONTROL_REQUEST = 0x0C
MAX_PAYLOAD = 64
MAX_CHUNK_BYTES = 56
BAUD_DEFAULT = 921600
REPLY_TIMEOUT_S = 1.5


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(message_type: int, sequence: int, payload: bytes) -> bytes:
    body = bytes([PROTOCOL_VERSION, message_type]) + struct.pack("<H", sequence) + \
        bytes([len(payload)]) + payload
    return FRAME_MAGIC + body + struct.pack("<H", crc16_ccitt(body))


class FrameScanner:
    """在混杂着控制台文本的字节流里找协议帧。"""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, chunk: bytes) -> list[tuple[int, int, bytes]]:
        self.buffer.extend(chunk)
        frames: list[tuple[int, int, bytes]] = []
        while True:
            start = self.buffer.find(FRAME_MAGIC)
            if start < 0:
                # 只保留可能是半个 magic 的尾巴，其余是控制台文本。
                if len(self.buffer) > 1:
                    del self.buffer[:-1]
                return frames
            if start > 0:
                del self.buffer[:start]
            if len(self.buffer) < 7:
                return frames
            payload_length = self.buffer[6]
            total = 9 + payload_length
            if payload_length > MAX_PAYLOAD:
                del self.buffer[:2]
                continue
            if len(self.buffer) < total:
                return frames
            candidate = bytes(self.buffer[:total])
            expected = struct.unpack_from("<H", candidate, total - 2)[0]
            if crc16_ccitt(candidate[2:total - 2]) != expected:
                del self.buffer[:2]
                continue
            message_type = candidate[3]
            sequence = struct.unpack_from("<H", candidate, 4)[0]
            frames.append((message_type, sequence, candidate[7:7 + payload_length]))
            del self.buffer[:total]


def read_response(scanner: FrameScanner, port: serial.Serial, sequence: int,
                  deadline: float):
    while time.monotonic() < deadline:
        frames = scanner.feed(port.read(4096))
        for message_type, frame_sequence, payload in frames:
            if message_type != MESSAGE_LOG_READ_RESPONSE or frame_sequence != sequence:
                continue
            if len(payload) < 8:
                continue
            offset, total = struct.unpack_from("<II", payload, 0)
            return offset, total, payload[8:]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="按协议下载某块板的板载滚动日志")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=BAUD_DEFAULT)
    parser.add_argument("--out", required=True, help="保存的日志文件路径")
    parser.add_argument("--clear-before", action="store_true",
                        help="取日志前先清空板载日志（默认保留）")
    parser.add_argument("--pause", action="store_true",
                        help="下载前暂停板载写盘（A/B 对照排查写盘对时序的影响）")
    parser.add_argument("--resume", action="store_true",
                        help="下载后恢复板载写盘")
    parser.add_argument("--clear-after", action="store_true",
                        help="取完日志后清空板载日志")
    parser.add_argument("--max-bytes", type=int, default=MAX_CHUNK_BYTES,
                        help="单块读取模式每次请求的字节数，1..56")
    parser.add_argument("--tail", type=int, default=0,
                        help="只取日志末尾 N 字节（0=全部；日志很长时用它直接看最新状态）")

    parser.add_argument("--max-total-bytes", type=int, default=1 << 20,
                        help="流式下载的总字节上限（默认 1 MB，足够覆盖 512 KB 轮转）")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="流式下载的最长等待时间（秒）")
    args = parser.parse_args()

    chunk_bytes = max(1, min(args.max_bytes, MAX_CHUNK_BYTES))
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.05
    # 显式放开 DTR/RTS：取日志不应该复位被测板。
    port.dtr = False
    port.rts = False
    try:
        port.open()
        port.dtr = False
        port.rts = False
    except serial.SerialException as error:
        print(f"打开 {args.port} 失败：{error}", file=sys.stderr)
        return 2
    port.reset_input_buffer()

    scanner = FrameScanner()
    sequence = 1

    def request(message_type: int, payload: bytes):
        nonlocal sequence
        port.write(build_frame(message_type, sequence, payload))
        port.flush()
        result = read_response(scanner, port, sequence, time.monotonic() + REPLY_TIMEOUT_S)
        sequence = (sequence + 1) & 0xFFFF
        return result

    try:
        if args.pause:
            paused = request(MESSAGE_LOG_CONTROL_REQUEST, b"\x01")
            print(f"[暂停] {'成功' if paused else '无响应'}")
        if args.clear_before:
            cleared = request(MESSAGE_LOG_CLEAR_REQUEST, b"")
            print(f"[清空] {'成功' if cleared else '无响应'}")

        # 需要尾部时先问一次 total，再从这里开始 dump。
        start_offset = 0
        if args.tail > 0:
            probe = request(MESSAGE_LOG_READ_REQUEST, struct.pack("<I", 0) + bytes([1]))
            if probe is not None:
                known_total = probe[1]
                if known_total > args.tail:
                    start_offset = known_total - args.tail
                    print(f"[尾部] 板端共 {known_total} 字节，从 {start_offset} 开始取")
        # 流式下载：一条请求换取连续多个分片；读到空分片即结束。
        collected = bytearray()
        total = None
        sequence_now = sequence
        port.write(build_frame(MESSAGE_LOG_DUMP_REQUEST, sequence_now,
                               struct.pack("<II", start_offset, args.max_total_bytes)))
        port.flush()
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            frames = scanner.feed(port.read(4096))
            finished = False
            for message_type, frame_sequence, payload in frames:
                if message_type != MESSAGE_LOG_READ_RESPONSE or \
                        frame_sequence != sequence_now or len(payload) < 8:
                    continue
                offset, total = struct.unpack_from("<II", payload, 0)
                data = payload[8:]
                if not data:
                    finished = True
                    break
                collected.extend(data)
            if finished:
                break
        sequence = (sequence + 1) & 0xFFFF
        if args.resume:
            request(MESSAGE_LOG_CONTROL_REQUEST, b"\x00")
            print("[恢复] 已发送")
        if args.clear_after:
            request(MESSAGE_LOG_CLEAR_REQUEST, b"")
    finally:
        port.close()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    text = collected.decode("utf-8", errors="replace")
    out_path.write_text(text, encoding="utf-8")
    print(f"[结果] 板载日志 {len(collected)} 字节 / 板端上报 {total} 字节 → {out_path}")
    return 0 if collected else 1


if __name__ == "__main__":
    raise SystemExit(main())
