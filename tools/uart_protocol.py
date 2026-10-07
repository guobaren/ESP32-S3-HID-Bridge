"""通用 UART 帧编码与扫描工具，供受控诊断脚本复用。"""

from __future__ import annotations

import struct
import time

FRAME_MAGIC = b"\xA5\x5A"
PROTOCOL_VERSION = 0x02
MAX_PAYLOAD = 64


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(message_type: int, sequence: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 超过 {MAX_PAYLOAD} 字节")
    body = bytes([PROTOCOL_VERSION, message_type]) + struct.pack("<H", sequence) + \
        bytes([len(payload)]) + payload
    return FRAME_MAGIC + body + struct.pack("<H", crc16_ccitt(body))


class FrameScanner:
    """在控制台文本或其他字节之间扫描合法协议帧。"""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, chunk: bytes) -> list[tuple[int, int, bytes]]:
        self.buffer.extend(chunk)
        frames: list[tuple[int, int, bytes]] = []
        while True:
            start = self.buffer.find(FRAME_MAGIC)
            if start < 0:
                if len(self.buffer) > 1:
                    del self.buffer[:-1]
                return frames
            if start > 0:
                del self.buffer[:start]
            if len(self.buffer) < 7:
                return frames
            payload_length = self.buffer[6]
            if payload_length > MAX_PAYLOAD:
                del self.buffer[:2]
                continue
            total = 9 + payload_length
            if len(self.buffer) < total:
                return frames
            candidate = bytes(self.buffer[:total])
            if candidate[2] != PROTOCOL_VERSION or \
                    crc16_ccitt(candidate[2:total - 2]) != struct.unpack_from("<H", candidate, total - 2)[0]:
                del self.buffer[:2]
                continue
            frames.append((candidate[3], struct.unpack_from("<H", candidate, 4)[0],
                           candidate[7:7 + payload_length]))
            del self.buffer[:total]


def read_response(scanner: FrameScanner, port, message_type: int, sequence: int, deadline: float):
    while time.monotonic() < deadline:
        for frame_type, frame_sequence, payload in scanner.feed(port.read(4096)):
            if frame_type == message_type and frame_sequence == sequence:
                return payload
    return None
