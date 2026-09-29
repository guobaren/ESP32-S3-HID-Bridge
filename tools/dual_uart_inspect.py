#!/usr/bin/env python3
"""双板 UART0 状态查看、诊断事件实时采集、Profile 备份/写入和移动探针。

只通过每块板自己的 UART0 工作。默认不发送鼠标/键盘输入；启动时只发
DEVICE_PROBE，并读取板载日志尾部。Profile 写入必须显式指定 --inject-profile。

每个端口生成三份采集文件：
  <端口>.uart0.rx.bin  串口驱动交付给本进程的原始接收字节
  <端口>.uart0.tx.bin  本工具发送的原始协议字节
  <端口>.jsonl         带主机时间、来源和解析字段的帧/文本行记录

事件流只覆盖固件接入的 USB/HID/UART1 观察点，不能视为完整 USB 总线抓包。UART0
事件/原始字节和板端 dropped 计数可帮助发现损失；主机驱动或板端采集之前的漏字节仍不可见。
"""

from __future__ import annotations

import argparse
import base64
import collections
import datetime as dt
import json
import re
import secrets
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Optional


MAGIC = b"\xA5\x5A"
PROTOCOL_VERSION = 0x02
MAX_PAYLOAD = 64
BAUD_DEFAULT = 921600
PROFILE_MAX_BYTES = 4096
PROFILE_READ_CHUNK = 56
PROFILE_WRITE_CHUNK = 60
ONBOARD_LOG_READ_CHUNK = 56
DEFAULT_BOOTSTRAP_TAIL_BYTES = 256 * 1024

TYPE_DEVICE_PROBE = 0x06
TYPE_DEVICE_HELLO = 0x07
TYPE_LOG_READ_REQUEST = 0x08
TYPE_LOG_READ_RESPONSE = 0x09
TYPE_LOG_DUMP_REQUEST = 0x0B
TYPE_DIAG_PROFILE_READ = 0x0D
TYPE_DIAG_PROFILE_DATA = 0x0E
TYPE_DIAG_PROFILE_BEGIN = 0x0F
TYPE_DIAG_PROFILE_CHUNK = 0x10
TYPE_DIAG_PROFILE_COMMIT = 0x11
TYPE_DIAG_PROFILE_RESULT = 0x12
TYPE_DIAG_PROFILE_MODE = 0x13
TYPE_DIAG_STREAM_CONTROL = 0x14
TYPE_DIAG_STREAM_EVENT = 0x15
TYPE_DIAG_STREAM_STATUS = 0x16
TYPE_DIAG_INJECT_REQUEST = 0x17
TYPE_DIAG_INJECT_RESULT = 0x18
# 强制重新采集并重新提议克隆（2026-09-28）：顶层 UART0 诊断命令，只在 M 板有意义。
TYPE_DIAG_PROFILE_REFRESH_REQUEST = 0x1C

FRAME_NAMES = {
    TYPE_DEVICE_PROBE: "DEVICE_PROBE",
    TYPE_DEVICE_HELLO: "DEVICE_HELLO",
    TYPE_LOG_READ_REQUEST: "LOG_READ_REQUEST",
    TYPE_LOG_READ_RESPONSE: "LOG_READ_RESPONSE",
    TYPE_LOG_DUMP_REQUEST: "LOG_DUMP_REQUEST",
    TYPE_DIAG_PROFILE_READ: "DIAG_PROFILE_READ",
    TYPE_DIAG_PROFILE_DATA: "DIAG_PROFILE_DATA",
    TYPE_DIAG_PROFILE_BEGIN: "DIAG_PROFILE_BEGIN",
    TYPE_DIAG_PROFILE_CHUNK: "DIAG_PROFILE_CHUNK",
    TYPE_DIAG_PROFILE_COMMIT: "DIAG_PROFILE_COMMIT",
    TYPE_DIAG_PROFILE_RESULT: "DIAG_PROFILE_RESULT",
    TYPE_DIAG_PROFILE_MODE: "DIAG_PROFILE_MODE",
    TYPE_DIAG_STREAM_CONTROL: "DIAG_STREAM_CONTROL",
    TYPE_DIAG_STREAM_EVENT: "DIAG_STREAM_EVENT",
    TYPE_DIAG_STREAM_STATUS: "DIAG_STREAM_STATUS",
    TYPE_DIAG_INJECT_REQUEST: "DIAG_INJECT_REQUEST",
    TYPE_DIAG_INJECT_RESULT: "DIAG_INJECT_RESULT",
}

ROLE_NAMES = {0: "UNRESOLVED", 1: "PC_DEVICE", 2: "MOUSE_HOST"}
DIAG_SOURCE_NAMES = {
    1: "P_USB",
    2: "M_USB",
    3: "UART1_RX",
    4: "UART1_TX",
    5: "UART0_RX",
}
DIAG_KIND_NAMES = {
    0x01: "KEYBOARD_REPORT",
    0x02: "MOUSE_REPORT",
    0x21: "PHYSICAL_MOUSE",
    0x27: "RAW_HID_INPUT",
    0x28: "HID_SET_REPORT",
    0x29: "HID_GET_REPORT_REQUEST",
    0x2A: "HID_GET_REPORT_RESPONSE",
    0x2B: "SOFTWARE_MOUSE",
    0x2D: "DEVICE_GONE",
    0x80: "P_SET_REPORT",
    0x81: "P_GET_REPORT_REQUEST",
    0x82: "P_GET_REPORT_RESULT",
    0x83: "P_VENDOR_SETUP",
    0x84: "M_RAW_HID_INPUT",
    0x85: "P_USB_REPORT_SUBMIT",
    0x86: "P_USB_REPORT_COMPLETE",
    0x87: "P_USB_REPORT_FAILED",
}


def diag_event_details(kind: int, data: bytes) -> dict[str, Any]:
    """Decode observation metadata without implying that a clipped USB payload is whole."""
    layouts = {
        0x80: (5, 6),  # SET_REPORT: original byte count, raw payload offset
        0x82: (5, 6),  # GET_REPORT result
        0x85: (3, 4),  # TinyUSB submit result
        0x86: (1, 2),  # completion callback
        0x87: (4, 5),  # failed callback transferred bytes
    }
    details: dict[str, Any] = {}
    if kind in layouts:
        length_index, data_offset = layouts[kind]
        if len(data) <= length_index:
            details.update(underlying_payload_truncated=True,
                           declared_payload_length=None, captured_payload_length=0)
        else:
            declared = data[length_index]
            captured = max(0, len(data) - data_offset)
            details.update(underlying_payload_truncated=declared > captured,
                           declared_payload_length=declared,
                           captured_payload_length=captured)
    if kind == 0x83:
        if len(data) >= 10:
            request_value, request_index, request_length = struct.unpack_from("<HHH", data, 3)
            details.update(
                callback_result="STALL (return false)",
                setup={"stage": data[0], "bmRequestType": data[1],
                       "bRequest": data[2], "wValue": request_value,
                       "wIndex": request_index, "wLength": request_length},
            )
        else:
            details["callback_result"] = "STALL (return false); truncated setup metadata"
    return details
INJECTION_RESULT_NAMES = {
    0: "accepted / queued",
    1: "invalid message, length, or chunk order",
    2: "wrong board role (P/PC_DEVICE required)",
    3: "no active injection or 10 second injection timeout",
    4: "length, CRC32, or Profile decode failed",
    5: "P-side reconfiguration could not be queued",
}

FRAME_HEADER_SIZE = 7
FRAME_OVERHEAD = 9
PROFILE_MAGIC = 0x50444948  # little-endian bytes "HIDP"
PROFILE_VERSION = 2
PROFILE_HEADER_SIZE = 20
PROFILE_REPORT_ENTRY_SIZE = 6

VENDOR_MARKERS = re.compile(
    r"(?i)(vendor|hid\+\+|hidpp|厂商|VENDOR_CONTROL|HID_SET_REPORT|HID_GET_REPORT)"
)
KEY_VALUE_RE = re.compile(r"(?P<key>[^\s=]+)=(?P<value>[^\s]+)")
VID_PID_RE = re.compile(r"VID:PID\s*=\s*([0-9A-Fa-f]{4})\s*:\s*([0-9A-Fa-f]{4})")
DEVICE_LOG_TIME_RE = re.compile(r"^\[b(?P<boot>\d+)\s+(?P<uptime>[^\]]+)\]")


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(message_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if not 0 <= message_type <= 0xFF:
        raise ValueError("message type must fit in u8")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload exceeds {MAX_PAYLOAD} bytes")
    body = bytes((PROTOCOL_VERSION, message_type)) + struct.pack("<H", sequence & 0xFFFF)
    body += bytes((len(payload),)) + payload
    return MAGIC + body + struct.pack("<H", crc16_ccitt(body))


@dataclass(frozen=True)
class Frame:
    version: int
    message_type: int
    sequence: int
    payload: bytes
    wire: bytes


@dataclass
class _DiagAssembly:
    event_id: int
    timestamp_us_low32: int
    source: int
    kind: int
    total_length: int
    data: bytearray
    received: bytearray
    received_count: int = 0
    fragment_offsets: set[int] = field(default_factory=set)


class DiagEventReassembler:
    """Rebuild 0x15 event fragments and account for malformed/incomplete events."""

    HEADER_SIZE = 12
    MAX_DATA = 64
    MAX_FRAGMENT_DATA = 52

    def __init__(self, max_pending: int = 256) -> None:
        self.max_pending = max_pending
        self.pending: collections.OrderedDict[int, _DiagAssembly] = collections.OrderedDict()
        self.completed = 0
        self.errors = 0
        self.evicted = 0

    def feed(self, payload: bytes) -> tuple[Optional[dict[str, Any]], list[str]]:
        errors: list[str] = []
        if len(payload) < self.HEADER_SIZE:
            self.errors += 1
            return None, ["fragment shorter than 12-byte event header"]
        event_id, timestamp, source, kind, total, offset = struct.unpack_from("<IIBBBB", payload)
        chunk = payload[self.HEADER_SIZE:]
        if total > self.MAX_DATA or offset > total or len(chunk) > self.MAX_FRAGMENT_DATA or \
                len(chunk) > total - offset or (total != 0 and not chunk):
            self.errors += 1
            return None, [f"invalid fragment bounds id={event_id} total={total} offset={offset} chunk={len(chunk)}"]
        if total == 0 and (offset != 0 or chunk):
            self.errors += 1
            return None, [f"invalid empty event fragment id={event_id}"]

        assembly = self.pending.get(event_id)
        if assembly is None:
            if len(self.pending) >= self.max_pending:
                old_id, old = self.pending.popitem(last=False)
                errors.append(f"pending event evicted id={old_id} received={old.received_count}/{old.total_length}")
                self.evicted += 1
            assembly = _DiagAssembly(event_id, timestamp, source, kind, total,
                                     bytearray(total), bytearray(total))
            self.pending[event_id] = assembly
        elif (assembly.timestamp_us_low32, assembly.source, assembly.kind,
              assembly.total_length) != (timestamp, source, kind, total):
            del self.pending[event_id]
            errors.append(f"fragment metadata conflict id={event_id}")
            self.errors += len(errors)
            return None, errors

        assembly.fragment_offsets.add(offset)
        if chunk:
            for index, value in enumerate(chunk, offset):
                if assembly.received[index]:
                    if assembly.data[index] != value:
                        del self.pending[event_id]
                        errors.append(f"overlapping fragment conflict id={event_id} offset={index}")
                        self.errors += len(errors)
                        return None, errors
                else:
                    assembly.data[index] = value
                    assembly.received[index] = 1
                    assembly.received_count += 1
        self.pending.move_to_end(event_id)
        if assembly.received_count != assembly.total_length:
            self.errors += len(errors)
            return None, errors

        del self.pending[event_id]
        self.completed += 1
        event = {
            "event_id": event_id,
            "timestamp_us_low32": timestamp,
            "source": source,
            "kind": kind,
            "data": bytes(assembly.data),
            "fragment_count": len(assembly.fragment_offsets),
        }
        self.errors += len(errors)
        return event, errors

    def finish(self) -> list[dict[str, int]]:
        incomplete = [
            {"event_id": item.event_id, "received_bytes": item.received_count,
             "total_length": item.total_length}
            for item in self.pending.values()
        ]
        self.pending.clear()
        return incomplete


class MixedStreamParser:
    """从控制台文本与二进制帧混杂的 UART0 字节流恢复完整帧和文本行。"""

    def __init__(self, on_line: Callable[[bytes, bool, bool], None],
                 max_text_fragment: int = 16 * 1024) -> None:
        self.buffer = bytearray()
        self.text_pending = bytearray()
        self.on_line = on_line
        self.max_text_fragment = max_text_fragment
        self.valid_frames = 0
        self.invalid_candidates = 0
        self.text_lines = 0
        self.truncated_text_fragments = 0
        self.incomplete_tail_bytes = 0

    def _emit_text(self, data: bytes) -> None:
        if not data:
            return
        self.text_pending.extend(data)
        while True:
            newline = self.text_pending.find(b"\n")
            if newline >= 0:
                line = bytes(self.text_pending[:newline]).rstrip(b"\r")
                del self.text_pending[:newline + 1]
                self._emit_line(line, True, False)
                continue
            if len(self.text_pending) > self.max_text_fragment:
                line = bytes(self.text_pending[:self.max_text_fragment])
                del self.text_pending[:self.max_text_fragment]
                self._emit_line(line, False, True)
                self.truncated_text_fragments += 1
                continue
            break

    def _emit_line(self, line: bytes, terminated: bool, truncated: bool) -> None:
        if line or terminated:
            self.text_lines += 1
            self.on_line(line, terminated, truncated)

    def feed(self, chunk: bytes) -> list[Frame]:
        if chunk:
            self.buffer.extend(chunk)
        frames: list[Frame] = []
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                # 保留可能跨 read() 边界的半个帧头，其余交给文本行解析器。
                keep = 1 if self.buffer.endswith(MAGIC[:1]) else 0
                emit_length = len(self.buffer) - keep
                if emit_length:
                    self._emit_text(bytes(self.buffer[:emit_length]))
                    del self.buffer[:emit_length]
                break
            if start:
                self._emit_text(bytes(self.buffer[:start]))
                del self.buffer[:start]
            if len(self.buffer) < FRAME_HEADER_SIZE:
                break
            payload_length = self.buffer[6]
            if payload_length > MAX_PAYLOAD:
                self.invalid_candidates += 1
                self._emit_text(bytes(self.buffer[:1]))
                del self.buffer[:1]
                continue
            total_length = FRAME_OVERHEAD + payload_length
            if len(self.buffer) < total_length:
                break
            candidate = bytes(self.buffer[:total_length])
            expected_crc = struct.unpack_from("<H", candidate, total_length - 2)[0]
            if crc16_ccitt(candidate[2:total_length - 2]) != expected_crc:
                self.invalid_candidates += 1
                # 只回退一个字节，允许在坏候选帧内部重新同步。
                self._emit_text(bytes(self.buffer[:1]))
                del self.buffer[:1]
                continue
            version = candidate[2]
            message_type = candidate[3]
            sequence = struct.unpack_from("<H", candidate, 4)[0]
            frames.append(Frame(version, message_type, sequence,
                                candidate[7:7 + payload_length], candidate))
            del self.buffer[:total_length]
            self.valid_frames += 1
        return frames

    def finish(self) -> None:
        if self.buffer:
            pending = bytes(self.buffer)
            self.incomplete_tail_bytes += len(pending)
            # 有帧头的未完整候选属于不完整帧；其他尾部仍按未结束文本保存。
            if not pending.startswith(MAGIC):
                self._emit_text(pending)
            self.buffer.clear()
        if self.text_pending:
            line = bytes(self.text_pending).rstrip(b"\r")
            self.text_pending.clear()
            self._emit_line(line, False, False)


@dataclass
class BoardStatus:
    port: str
    role: Optional[int] = None
    role_verified: bool = False
    role_source: Optional[str] = None
    role_seen_at: Optional[str] = None
    current_mouse: Optional[dict[str, Any]] = None
    copied_profile: Optional[dict[str, Any]] = None
    clone: Optional[dict[str, Any]] = None
    clone_outcome: Optional[str] = None
    uart0_stats: Optional[dict[str, Any]] = None
    uart1_stats: Optional[dict[str, Any]] = None
    hid_stats: Optional[dict[str, Any]] = None
    queue_metrics: dict[str, dict[str, Any]] = field(default_factory=dict)
    latest_vendor_line: Optional[dict[str, Any]] = None
    clone_outcome_monotonic: Optional[float] = None
    source_precedence: dict[str, int] = field(default_factory=dict)

    def note_role(self, role: int, source: str, verified: bool, observed_at: str) -> None:
        if role not in ROLE_NAMES:
            return
        if verified or self.role is None or not self.role_verified:
            self.role = role
            self.role_verified = verified or self.role_verified
            self.role_source = source
            self.role_seen_at = observed_at


def parse_device_hello(frame: Frame, accepted_nonces: set[bytes]) -> Optional[int]:
    if frame.version != PROTOCOL_VERSION or frame.message_type != TYPE_DEVICE_HELLO:
        return None
    payload = frame.payload
    if len(payload) != 17 or payload[:8] != b"HIDBRDG2":
        return None
    if payload[8:16] not in accepted_nonces:
        return None
    return payload[16]


def _kv_fields(text: str) -> dict[str, str]:
    return {match.group("key"): match.group("value") for match in KEY_VALUE_RE.finditer(text)}


def parse_profile_blob(blob: bytes) -> dict[str, Any]:
    """解析 Profile v2 的边界与基本身份字段，不改写输入字节。"""
    if len(blob) < PROFILE_HEADER_SIZE:
        raise ValueError(f"Profile 短于固定头（{len(blob)} < {PROFILE_HEADER_SIZE}）")
    (magic, version, header_length, device_length, config_length, manufacturer_length,
     product_length, serial_length, report_count, flags) = struct.unpack_from(
        "<IHHHHHHHBB", blob, 0)
    if magic != PROFILE_MAGIC or version != PROFILE_VERSION:
        raise ValueError(f"未知 Profile magic/version: 0x{magic:08X}/{version}")
    if header_length != PROFILE_HEADER_SIZE:
        raise ValueError(f"Profile header_length={header_length}，预期 {PROFILE_HEADER_SIZE}")
    if report_count > 8:
        raise ValueError(f"report descriptor count 超限: {report_count}")

    cursor = header_length
    sections: dict[str, bytes] = {}
    for name, length in (
        ("device_descriptor", device_length),
        ("config_descriptor", config_length),
        ("manufacturer", manufacturer_length),
        ("product", product_length),
        ("serial", serial_length),
    ):
        end = cursor + length
        if end > len(blob):
            raise ValueError(f"Profile {name} 被截断：需要 {end} 字节，实际 {len(blob)}")
        sections[name] = blob[cursor:end]
        cursor = end

    reports = []
    for index in range(report_count):
        if cursor + PROFILE_REPORT_ENTRY_SIZE > len(blob):
            raise ValueError(f"Profile report entry {index} 被截断")
        interface_number, subclass, protocol, _reserved, report_length = struct.unpack_from(
            "<BBBBH", blob, cursor)
        cursor += PROFILE_REPORT_ENTRY_SIZE
        end = cursor + report_length
        if end > len(blob):
            raise ValueError(f"Report descriptor {index} 被截断")
        reports.append({
            "interface": interface_number,
            "subclass": subclass,
            "protocol": protocol,
            "length": report_length,
        })
        cursor = end
    if cursor != len(blob):
        raise ValueError(f"Profile 有 {len(blob) - cursor} 个尾随字节")

    device = sections["device_descriptor"]
    vid = pid = None
    if len(device) >= 12:
        vid, pid = struct.unpack_from("<HH", device, 8)

    def decode_string(key: str) -> str:
        return sections[key].decode("utf-8", errors="replace")

    return {
        "version": version,
        "length": len(blob),
        "vid": f"{vid:04X}" if vid is not None else None,
        "pid": f"{pid:04X}" if pid is not None else None,
        "vid_pid": f"{vid:04X}:{pid:04X}" if vid is not None and pid is not None else None,
        "device_descriptor_length": device_length,
        "config_descriptor_length": config_length,
        "manufacturer": decode_string("manufacturer"),
        "product": decode_string("product"),
        "serial": decode_string("serial"),
        "report_descriptor_count": report_count,
        "report_descriptors": reports,
        "flags": flags,
        "partial": bool(flags & 0x01),
    }


def parse_log_line(text: str, source: str, observed_at: str,
                   observed_monotonic: float) -> dict[str, Any]:
    """把固件可见日志映射为可检索状态字段；原文始终保留。"""
    fields = _kv_fields(text)
    match = VID_PID_RE.search(text)
    vid_pid = f"{match.group(1).upper()}:{match.group(2).upper()}" if match else None
    boot_time = DEVICE_LOG_TIME_RE.match(text)
    device_time = ({"boot": int(boot_time.group("boot")), "uptime": boot_time.group("uptime")}
                   if boot_time else None)
    record: dict[str, Any] = {
        "source": source,
        "observed_at_utc": observed_at,
        "device_time": device_time,
        "text": text,
        "fields": fields,
        "vendor_related": bool(VENDOR_MARKERS.search(text)),
    }
    if vid_pid:
        record["vid_pid"] = vid_pid
    return record


class PortSession:
    def __init__(self, port_name: str, output_directory: Path, start_monotonic: float,
                 tag: str = "") -> None:
        self.port_name = port_name
        self.start_monotonic = start_monotonic
        suffix = f"-{tag}" if tag else ""
        stem = f"{sanitize_filename(port_name)}{suffix}"
        self.rx_path = output_directory / f"{stem}.uart0.rx.bin"
        self.tx_path = output_directory / f"{stem}.uart0.tx.bin"
        self.jsonl_path = output_directory / f"{stem}.jsonl"
        output_directory.mkdir(parents=True, exist_ok=True)
        self.rx_file = self.rx_path.open("wb")
        self.tx_file = self.tx_path.open("wb")
        self.jsonl_file = self.jsonl_path.open("w", encoding="utf-8", newline="\n")
        self.port: Any = None
        self.status = BoardStatus(port_name)
        self.parser = MixedStreamParser(self._on_console_bytes)
        self.pending_frames: collections.deque[Frame] = collections.deque()
        self.recent_diag_events: collections.deque[dict[str, Any]] = collections.deque(maxlen=8192)
        self.accepted_probe_nonces: set[bytes] = set()
        self.sequence = 1
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.invalid_frame_candidates = 0
        self.device_hello_count = 0
        self.vendor_log_lines = 0
        self.console_lines = 0
        self.pending_frame_drops = 0
        self.diag_reassembler = DiagEventReassembler()
        self.diag_fragments_received = 0
        self.diag_incomplete_events: list[dict[str, int]] = []
        self.diag_stream_status: Optional[dict[str, int]] = None
        self.diag_status_errors = 0
        self.diag_subscribed = False
        self.serial_error: Optional[str] = None
        self.snapshot_info: Optional[dict[str, Any]] = None
        self.action_info: Optional[dict[str, Any]] = None
        self.read_chunk_size = 4096
        self._closed = False

    def _now(self) -> tuple[str, float, float]:
        wall = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
        monotonic = time.monotonic()
        return wall, (monotonic - self.start_monotonic) * 1000.0, monotonic

    def write_json(self, record: dict[str, Any]) -> None:
        wall, relative_ms, _ = self._now()
        output = {"observed_at_utc": wall, "relative_ms": round(relative_ms, 3),
                  "port": self.port_name, **record}
        self.jsonl_file.write(json.dumps(output, ensure_ascii=False, separators=(",", ":")) + "\n")
        self.jsonl_file.flush()

    def write_tx(self, message_type: int, sequence: int, payload: bytes) -> int:
        wire = build_frame(message_type, sequence, payload)
        written = self.port.write(wire)
        if written != len(wire):
            raise OSError(f"UART0 写帧不完整：{written}/{len(wire)} bytes")
        self.port.flush()
        self.tx_file.write(wire)
        self.tx_file.flush()
        self.tx_bytes += len(wire)
        record: dict[str, Any] = {
            "record_type": "frame",
            "direction": "tx",
            "message_type": FRAME_NAMES.get(message_type, f"0x{message_type:02X}"),
            "message_type_id": message_type,
            "sequence": sequence,
            "payload_length": len(payload),
            "wire_length": len(wire),
            "payload_hex": payload.hex() if len(payload) <= 24 else None,
        }
        self.write_json(record)
        return sequence

    def send_frame(self, message_type: int, payload: bytes = b"") -> int:
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFF
        return self.write_tx(message_type, sequence, payload)

    def _diag_event_record(self, event: dict[str, Any]) -> None:
        self.recent_diag_events.append({**event, "observed_monotonic": time.monotonic()})
        source = event["source"]
        kind = event["kind"]
        source_name = DIAG_SOURCE_NAMES.get(source, f"SOURCE_{source}")
        kind_name = DIAG_KIND_NAMES.get(
            kind, FRAME_NAMES.get(kind, f"0x{kind:02X}"))
        data = event["data"]
        details = diag_event_details(kind, data)
        role = ROLE_NAMES.get(self.status.role, "UNKNOWN")
        wall, _, _ = self._now()
        truncation = " truncated=true" if details.get("underlying_payload_truncated") else ""
        callback_result = f" result={details['callback_result']}" \
            if "callback_result" in details else ""
        print(f"[{role}/{self.port_name} {wall} board_us={event['timestamp_us_low32']}] "
              f"event_id={event['event_id']} source={source_name} kind={kind_name} "
              f"len={len(data)} hex={data.hex()}{truncation}{callback_result}", flush=True)
        self.write_json({
            "record_type": "diag_event",
            "event_id": event["event_id"],
            "timestamp_us_low32": event["timestamp_us_low32"],
            "source": source,
            "source_name": source_name,
            "kind": kind,
            "kind_name": kind_name,
            "data_length": len(data),
            "data_hex": data.hex(),
            "fragment_count": event["fragment_count"],
            **details,
        })

    def _diag_frame_record(self, frame: Frame) -> None:
        if frame.message_type == TYPE_DIAG_STREAM_EVENT:
            self.diag_fragments_received += 1
            event, errors = self.diag_reassembler.feed(frame.payload)
            for message in errors:
                self.write_json({"record_type": "diag_event_error", "error": message})
            if event is not None:
                self._diag_event_record(event)
            return
        if frame.message_type == TYPE_DIAG_STREAM_STATUS:
            if len(frame.payload) != 8:
                self.diag_status_errors += 1
                self.write_json({"record_type": "diag_stream_status_error",
                                 "payload_length": len(frame.payload),
                                 "payload_hex": frame.payload.hex()})
                return
            captured, dropped = struct.unpack("<II", frame.payload)
            self.diag_stream_status = {"captured": captured, "dropped": dropped}
            self.write_json({"record_type": "diag_stream_status",
                             "captured": captured, "dropped": dropped,
                             "sequence": frame.sequence})
            role = ROLE_NAMES.get(self.status.role, "UNKNOWN")
            print(f"[{role}/{self.port_name}] DIAG_STATUS captured={captured} dropped={dropped}",
                  flush=True)

    def _frame_record(self, frame: Frame) -> None:
        wall, relative_ms, monotonic = self._now()
        self._diag_frame_record(frame)
        if frame.message_type in (TYPE_DIAG_STREAM_EVENT, TYPE_DIAG_STREAM_STATUS):
            return
        role = parse_device_hello(frame, self.accepted_probe_nonces)
        if role is not None:
            self.device_hello_count += 1
            self.status.note_role(role, "DEVICE_HELLO nonce match", True, wall)
        type_name = FRAME_NAMES.get(frame.message_type, f"0x{frame.message_type:02X}")
        record: dict[str, Any] = {
            "record_type": "frame",
            "direction": "rx",
            "message_type": type_name,
            "message_type_id": frame.message_type,
            "sequence": frame.sequence,
            "protocol_version": frame.version,
            "payload_length": len(frame.payload),
            "wire_length": len(frame.wire),
        }
        if frame.message_type == TYPE_DEVICE_HELLO and len(frame.payload) >= 17:
            record["hello_signature"] = frame.payload[:8].decode("ascii", errors="replace")
            record["nonce_match"] = frame.payload[8:16] in self.accepted_probe_nonces
            record["role_id"] = frame.payload[16]
            record["role"] = ROLE_NAMES.get(frame.payload[16], "UNKNOWN")
        # 日志/设备 Profile 数据会含完整历史日志、序列号和描述符；原始内容只放在
        # RX 二进制文件，避免 JSONL 再复制一个不可检索的 base64 大字段。
        if frame.message_type not in (TYPE_LOG_READ_RESPONSE, TYPE_DIAG_PROFILE_DATA,
                                      TYPE_DIAG_STREAM_EVENT):
            record["payload_hex"] = frame.payload.hex()
        self.write_json({"record_type": "frame", "direction": "rx",
                         "message_type": type_name, "message_type_id": frame.message_type,
                         "sequence": frame.sequence, "protocol_version": frame.version,
                         "payload_length": len(frame.payload), "wire_length": len(frame.wire),
                         **({"hello_signature": record["hello_signature"],
                            "nonce_match": record["nonce_match"],
                            "role_id": record["role_id"], "role": record["role"]}
                           if "hello_signature" in record else {}),
                         **({"payload_hex": record["payload_hex"]}
                            if "payload_hex" in record else {})})
        if frame.message_type in (TYPE_DIAG_STREAM_EVENT, TYPE_DIAG_STREAM_STATUS):
            return
        if len(self.pending_frames) >= 256:
            self.pending_frames.popleft()
            self.pending_frame_drops += 1
        self.pending_frames.append(frame)

    def _on_console_bytes(self, raw_line: bytes, terminated: bool, truncated: bool) -> None:
        self.record_console_line(raw_line, "uart0_live", terminated, truncated)

    def record_console_line(self, raw_line: bytes, source: str, terminated: bool,
                            truncated: bool, snapshot: Optional[dict[str, Any]] = None) -> None:
        wall, relative_ms, monotonic = self._now()
        text = raw_line.decode("utf-8", errors="replace")
        parsed = parse_log_line(text, source, wall, monotonic)
        role_before = self.status.role
        self._update_status(text, parsed, monotonic)
        vendor_related = parsed["vendor_related"]
        if vendor_related:
            self.vendor_log_lines += 1
            self.status.latest_vendor_line = {
                "observed_at_utc": wall, "source": source, "text": text,
                "raw_line_base64": base64.b64encode(raw_line).decode("ascii"),
            }
        self.console_lines += 1
        record: dict[str, Any] = {
            "record_type": "console_line",
            "source": source,
            "kind": "vendor_log" if vendor_related else "console_log",
            "text": text,
            "raw_line_base64": base64.b64encode(raw_line).decode("ascii"),
            "line_terminated": terminated,
            "truncated": truncated,
            "vendor_related": vendor_related,
            "parsed": parsed,
        }
        if snapshot is not None:
            record["snapshot"] = snapshot
        if role_before != self.status.role:
            record["role_inference"] = ROLE_NAMES.get(self.status.role, "UNKNOWN")
        self.write_json(record)

    def _update_status(self, text: str, parsed: dict[str, Any], monotonic: float) -> None:
        source = parsed["source"]
        priority = 2 if source == "uart0_live" else 1
        fields = parsed["fields"]
        wall = parsed["observed_at_utc"]
        if self.status.role is None or not self.status.role_verified:
            if "dual_pc_hid" in text or "PC HID统计" in text or \
                    "PC VENDOR_CONTROL" in text or "动态USB严格克隆" in text or \
                    "收到物理HID Profile观察快照" in text:
                self.status.note_role(1, f"log inference ({source})", False, wall)
            elif "dual_hid_host" in text or "Host HID统计" in text or "HID接口发现" in text:
                self.status.note_role(2, f"log inference ({source})", False, wall)

        if "HID接口发现" in text and parsed.get("vid_pid"):
            self._set_status_value("current_mouse", {
                "vid_pid": parsed["vid_pid"],
                "address": _int_field(fields, "addr"),
                "interface": _int_field(fields, "interface"),
                "subclass": _int_field(fields, "subclass"),
                "protocol": _int_field(fields, "protocol"),
                "source": source,
                "observed_at_utc": wall,
                "device_time": parsed["device_time"],
                "raw_line": text,
            }, "current_mouse", priority)

        if "收到物理HID Profile观察快照" in text and parsed.get("vid_pid"):
            self._set_status_value("copied_profile", {
                "vid_pid": parsed["vid_pid"],
                "manufacturer_length": _int_field(fields, "manufacturer_len"),
                "product_length": _int_field(fields, "product_len"),
                "serial_present": fields.get("serial_present"),
                "interfaces": _int_field(fields, "interfaces"),
                "length_flags": fields.get("length_flags"),
                "source": source,
                "observed_at_utc": wall,
                "device_time": parsed["device_time"],
                "raw_line": text,
                "freshness": "latest observation in source log; may be stale",
            }, "copied_profile", priority)

        if "动态USB严格克隆已启用" in text and parsed.get("vid_pid"):
            self._set_status_value("clone", {
                "vid_pid": parsed["vid_pid"],
                "hid_count": _int_field(fields, "HID"),
                "mouse_instance": _int_field(fields, "mouse_instance"),
                "cdc": fields.get("CDC"),
                "source": source,
                "observed_at_utc": wall,
                "device_time": parsed["device_time"],
                "raw_line": text,
                "freshness": "latest successful clone log; may be stale",
            }, "clone", priority)
            self.status.clone_outcome = "mount_log_seen"
            self.status.clone_outcome_monotonic = monotonic
        elif "动态USB克隆启动失败" in text or "动态USB克隆排队失败" in text:
            self.status.clone_outcome = "failure_log_seen"
            self.status.clone_outcome_monotonic = monotonic
            self.write_json({"record_type": "clone_outcome", "outcome": "failure_log_seen",
                             "source": source, "raw_line": text})

        if "旧Profile、描述符、鼠标报告模板及厂商HID会话已清空" in text:
            self._set_status_value("clone", None, "clone", priority)

        if "UART0协议统计" in text:
            self._set_status_value("uart0_stats", {
                "fields": fields, "source": source, "observed_at_utc": wall, "raw_line": text,
            }, "uart0_stats", priority)
        if "UART1统计" in text:
            self._set_status_value("uart1_stats", {
                "fields": fields, "source": source, "observed_at_utc": wall, "raw_line": text,
            }, "uart1_stats", priority)
        if "Host HID统计" in text or "HID统计：rate" in text or "HID统计: rate" in text:
            self._set_status_value("hid_stats", {
                "fields": fields, "source": source, "observed_at_utc": wall, "raw_line": text,
            }, "hid_stats", priority)
        if "QUEUE name=" in text and fields.get("name"):
            queue_name = fields["name"]
            metrics = {
                key: _int_field(fields, key)
                for key in ("received", "rejected", "dropped", "peak", "consumed", "overflow")
                if _int_field(fields, key) is not None
            }
            self.status.queue_metrics[queue_name] = {
                **metrics,
                "source": source,
                "observed_at_utc": wall,
                "raw_line": text,
            }

    def _set_status_value(self, name: str, value: Any, precedence_key: str, priority: int) -> None:
        old_priority = self.status.source_precedence.get(precedence_key, -1)
        if priority >= old_priority:
            setattr(self.status, name, value)
            self.status.source_precedence[precedence_key] = priority

    def poll(self) -> None:
        if self.port is None or self._closed:
            return
        try:
            chunk = self.port.read(self.read_chunk_size)
        except Exception as error:  # serial library errors differ by backend/OS
            self.serial_error = str(error)
            self.write_json({"record_type": "serial_error", "error": self.serial_error})
            return
        if not chunk:
            return
        self.rx_bytes += len(chunk)
        self.rx_file.write(chunk)
        self.rx_file.flush()
        frames = self.parser.feed(chunk)
        for frame in frames:
            self._frame_record(frame)

    def take_frame(self, sequence: int, message_types: Iterable[int]) -> Optional[Frame]:
        accepted = set(message_types)
        for index, frame in enumerate(self.pending_frames):
            if frame.sequence == sequence and frame.message_type in accepted:
                del self.pending_frames[index]
                return frame
        return None

    def add_nonce(self, nonce: bytes) -> None:
        self.accepted_probe_nonces.add(nonce)

    def flush_pending_text(self) -> None:
        self.parser.finish()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self.flush_pending_text()
        self.diag_incomplete_events = self.diag_reassembler.finish()
        for incomplete in self.diag_incomplete_events:
            self.write_json({"record_type": "diag_event_incomplete", **incomplete,
                             "reason": "capture ended before all fragments arrived"})
        for file in (self.rx_file, self.tx_file, self.jsonl_file):
            try:
                file.flush()
                file.close()
            except OSError:
                pass
        if self.port is not None:
            try:
                self.port.close()
            except Exception:
                pass

    def summary(self) -> dict[str, Any]:
        return {
            "port": self.port_name,
            "role": ROLE_NAMES.get(self.status.role, "UNKNOWN"),
            "role_verified": self.status.role_verified,
            "role_source": self.status.role_source,
            "role_seen_at_utc": self.status.role_seen_at,
            "current_mouse": self.status.current_mouse,
            "copied_profile_latest_observation": self.status.copied_profile,
            "clone_latest_success_log": self.status.clone,
            "clone_outcome": self.status.clone_outcome,
            "clone_outcome_monotonic": self.status.clone_outcome_monotonic,
            "uart0_stats": self.status.uart0_stats,
            "uart1_stats": self.status.uart1_stats,
            "hid_stats": self.status.hid_stats,
            "queue_metrics": self.status.queue_metrics,
            "diag_stream_status": self.diag_stream_status,
            "diag_stream_subscribed_at_end": self.diag_subscribed,
            "latest_vendor_line": self.status.latest_vendor_line,
            "onboard_log_snapshot": self.snapshot_info,
            "action": self.action_info,
            "capture": {
                "rx_bytes_written": self.rx_bytes,
                "tx_bytes_written": self.tx_bytes,
                "parsed_frames": self.parser.valid_frames,
                "invalid_frame_candidates": self.parser.invalid_candidates,
                "incomplete_tail_bytes": self.parser.incomplete_tail_bytes,
                "console_lines": self.console_lines,
                "vendor_log_lines": self.vendor_log_lines,
                "truncated_console_fragments": self.parser.truncated_text_fragments,
                "pending_frame_queue_drops": self.pending_frame_drops,
                "diag_fragments_received": self.diag_fragments_received,
                "diag_events_reassembled": self.diag_reassembler.completed,
                "diag_event_reassembly_errors": self.diag_reassembler.errors,
                "diag_events_incomplete_at_end": self.diag_incomplete_events,
                "diag_stream_status_errors": self.diag_status_errors,
                "serial_error": self.serial_error,
                "rx_file": str(self.rx_path),
                "tx_file": str(self.tx_path),
                "jsonl_file": str(self.jsonl_path),
                "uart0_loss_note": (
                    "Only bytes returned by the host serial driver are recorded. ESP32/CH340/"
                    "driver-level drops and unobserved gaps cannot be quantified from this capture."
                ),
            },
        }


def _int_field(fields: dict[str, str], name: str) -> Optional[int]:
    value = fields.get(name)
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        try:
            return int(value, 10)
        except ValueError:
            return None


def sanitize_filename(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._") or "uart0"


def open_serial_port(port_name: str, baud: int) -> Any:
    try:
        import serial  # type: ignore
    except ImportError as error:
        raise RuntimeError("缺少 pyserial：请在当前 Python 环境安装 pyserial") from error
    port = serial.Serial(
        port=None,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.03,
        write_timeout=2.0,
        rtscts=False,
        dsrdtr=False,
    )
    # 尽量避免打开 CH340 时拉动自动复位脚；某些驱动仍可能在 open() 时短暂切换线态。
    port.dtr = False
    port.rts = False
    port.port = port_name
    port.open()
    port.dtr = False
    port.rts = False
    return port


def pump_sessions(sessions: list[PortSession]) -> None:
    for session in sessions:
        session.poll()


def set_diag_stream(sessions: list[PortSession], enabled: bool) -> None:
    value = b"\x01" if enabled else b"\x00"
    for session in sessions:
        if session.port is None or session._closed:
            continue
        session.send_frame(TYPE_DIAG_STREAM_CONTROL, value)
        session.diag_subscribed = enabled


def mouse_move_payload(x: int, y: int) -> bytes:
    if not -32768 <= x <= 32767 or not -32768 <= y <= 32767 or (x == 0 and y == 0):
        raise ValueError("移动 X/Y 必须为 int16，且不能同时为 0")
    return struct.pack("<Bhhbb", 0, x, y, 0, 0)


def run_move_probe(sessions: list[PortSession], mouse_board: PortSession,
                   payload: bytes, duration_s: float = 3.0) -> dict[str, bool]:
    """用现有软件输入通道注入 M，并实时确认各个可见关口。"""
    pc_boards = [item for item in sessions if item.status.role_verified and item.status.role == 1]
    pc_board = pc_boards[0] if len(pc_boards) == 1 else None
    stages = {"M_UART0_RX": False, "M_UART1_TX": False,
              "P_UART1_RX": False, "P_USB_SUBMIT": False, "P_USB_COMPLETE": False}
    cursors = {item.port_name: len(item.recent_diag_events) for item in sessions}
    started = time.monotonic()
    print(f"[移动探针] M={mouse_board.port_name} payload={payload.hex()}，观察 {duration_s:.1f}s")
    mouse_board.send_frame(0x05)  # SESSION_START；不占用其他输入会话。
    mouse_board.send_frame(0x02, payload)
    release_due = time.monotonic() + 0.25
    released = False
    try:
        while time.monotonic() - started < duration_s:
            pump_sessions(sessions)
            if not released and time.monotonic() >= release_due:
                mouse_board.send_frame(0x03)  # RELEASE_ALL，避免按键状态滞留。
                released = True
            for board in sessions:
                events = list(board.recent_diag_events)
                cursor = cursors[board.port_name]
                if cursor > len(events):
                    cursor = 0  # deque 环绕后重新扫描，仍按观察时间过滤。
                for event in events[cursor:]:
                    if event["observed_monotonic"] < started:
                        continue
                    kind, source, data = event["kind"], event["source"], event["data"]
                    stage = None
                    if board is mouse_board and source == 5 and kind == 0x02 and data == payload:
                        stage = "M_UART0_RX"
                    elif board is mouse_board and source == 4 and kind == 0x2B and data == payload:
                        stage = "M_UART1_TX"
                    elif board is pc_board and source == 3 and kind == 0x2B and data == payload:
                        stage = "P_UART1_RX"
                    elif board is pc_board and source == 1 and kind == 0x85 and data[:1] == b"\x01":
                        stage = "P_USB_SUBMIT"
                    elif board is pc_board and source == 1 and kind == 0x86:
                        stage = "P_USB_COMPLETE"
                    if stage and not stages[stage]:
                        stages[stage] = True
                        print(f"[移动探针] {stage} 已观察到，event_id={event['event_id']}", flush=True)
                cursors[board.port_name] = len(events)
            time.sleep(0.002)
    finally:
        if not released:
            mouse_board.send_frame(0x03)
    for stage, seen in stages.items():
        print(f"[移动探针] {stage}: {'Observed' if seen else 'Missing'}")
    print("[移动探针] P USB 提交/完成按时间窗匹配；无跨板 trace ID，不能单凭此证明目标程序消费。")
    return stages


def wait_for_frame(sessions: list[PortSession], target: PortSession, sequence: int,
                   message_types: Iterable[int], timeout_s: float) -> Optional[Frame]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        pump_sessions(sessions)
        frame = target.take_frame(sequence, message_types)
        if frame is not None:
            return frame
        time.sleep(0.005)
    pump_sessions(sessions)
    return target.take_frame(sequence, message_types)


def identify_boards(sessions: list[PortSession], timeout_s: float) -> None:
    next_probe: dict[str, float] = {}
    for session in sessions:
        nonce = secrets.token_bytes(8)
        session.add_nonce(nonce)
        session.send_frame(TYPE_DEVICE_PROBE, nonce)
        next_probe[session.port_name] = time.monotonic() + 0.45
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline and any(not s.status.role_verified for s in sessions):
        pump_sessions(sessions)
        now = time.monotonic()
        for session in sessions:
            if not session.status.role_verified and now >= next_probe[session.port_name]:
                nonce = secrets.token_bytes(8)
                session.add_nonce(nonce)
                session.send_frame(TYPE_DEVICE_PROBE, nonce)
                next_probe[session.port_name] = now + 0.45
        time.sleep(0.005)
    for session in sessions:
        role = ROLE_NAMES.get(session.status.role, "UNKNOWN")
        if session.status.role_verified:
            print(f"[识别] {session.port_name} -> {role}（DEVICE_HELLO nonce 匹配）")
        else:
            print(f"[识别] {session.port_name} -> 未识别；日志标签推断仅作参考")
    duplicate_roles = [role for role in (1, 2)
                       if sum(s.status.role == role and s.status.role_verified for s in sessions) > 1]
    if duplicate_roles:
        print("[风险] 两个串口被识别为同一角色；请核对 COM 端口和连接板卡。", file=sys.stderr)


def decode_log_response(frame: Frame) -> tuple[int, int, bytes]:
    if frame.message_type != TYPE_LOG_READ_RESPONSE or len(frame.payload) < 8:
        raise ValueError("LOG_READ_RESPONSE payload 短于 8 字节")
    offset, total = struct.unpack_from("<II", frame.payload, 0)
    return offset, total, frame.payload[8:]


def _parse_bootstrap_bytes(session: PortSession, data: bytes, snapshot: dict[str, Any]) -> None:
    parts = data.split(b"\n")
    if not data.endswith(b"\n") and parts:
        final = parts.pop()
    else:
        final = b""
    for index, raw_line in enumerate(parts):
        if raw_line.endswith(b"\r"):
            raw_line = raw_line[:-1]
        session.record_console_line(raw_line, "onboard_log_snapshot", True,
                                    snapshot.get("prefix_partial", False) and index == 0,
                                    snapshot)
    if final:
        session.record_console_line(final, "onboard_log_snapshot", False, True, snapshot)


def bootstrap_recent_logs(sessions: list[PortSession], session: PortSession,
                          tail_bytes: int, timeout_s: float) -> dict[str, Any]:
    result: dict[str, Any] = {
        "status": "failed",
        "source": "LOG_DUMP_REQUEST over this board UART0",
        "requested_tail_bytes": tail_bytes,
        "bytes_received": 0,
        "prefix_partial": False,
        "complete": False,
    }
    try:
        probe_sequence = session.send_frame(TYPE_LOG_READ_REQUEST, struct.pack("<IB", 0, 1))
        response = wait_for_frame(sessions, session, probe_sequence,
                                  (TYPE_LOG_READ_RESPONSE,), timeout_s)
        if response is None:
            result["reason"] = "log-size probe timed out"
            session.snapshot_info = result
            return result
        _offset, total, _data = decode_log_response(response)
        result["total_bytes_reported"] = total
        start_offset = max(0, total - tail_bytes)
        expected_bytes = min(tail_bytes, max(0, total - start_offset))
        result["start_offset"] = start_offset
        result["older_bytes_omitted"] = start_offset
        if expected_bytes == 0:
            result.update(status="empty", complete=True)
            session.snapshot_info = result
            return result

        dump_sequence = session.send_frame(TYPE_LOG_DUMP_REQUEST,
                                           struct.pack("<II", start_offset, tail_bytes))
        received = bytearray()
        next_offset = start_offset
        total_changed = False
        ended = False
        # 日志传输速率与闪存读速会变；超时留足裕量，同时有界退出。
        dump_timeout = max(timeout_s, 6.0 + expected_bytes / 12000.0)
        deadline = time.monotonic() + dump_timeout
        while time.monotonic() < deadline:
            pump_sessions(sessions)
            while True:
                frame = session.take_frame(dump_sequence, (TYPE_LOG_READ_RESPONSE,))
                if frame is None:
                    break
                offset, current_total, data = decode_log_response(frame)
                total_changed |= current_total != total
                if not data:
                    ended = True
                    break
                if offset != next_offset:
                    result["offset_mismatch"] = {"expected": next_offset, "received": offset}
                    ended = True
                    break
                if len(data) > ONBOARD_LOG_READ_CHUNK:
                    result["oversized_chunk"] = len(data)
                    ended = True
                    break
                received.extend(data)
                next_offset += len(data)
            if ended:
                break
            time.sleep(0.002)
        result["bytes_received"] = len(received)
        result["end_marker_received"] = ended and "offset_mismatch" not in result and \
            "oversized_chunk" not in result
        result["reported_total_changed_during_dump"] = total_changed
        result["prefix_partial"] = start_offset > 0 and bool(received) and received[:1] != b"\n"
        result["complete"] = (result["end_marker_received"] and
                               len(received) == expected_bytes and
                               "offset_mismatch" not in result and
                               "oversized_chunk" not in result)
        result["status"] = "complete" if result["complete"] else "partial"
        if not result["complete"]:
            result["reason"] = "stream ended early, offset mismatch, or timeout"
        result["older_bytes_omitted"] = start_offset
        snapshot = dict(result)
        _parse_bootstrap_bytes(session, bytes(received), snapshot)
    except (OSError, ValueError, struct.error) as error:
        result["reason"] = str(error)
    session.snapshot_info = result
    return result


def ensure_role(session: PortSession, role: int) -> None:
    if not session.status.role_verified:
        raise RuntimeError(f"{session.port_name} 未通过 DEVICE_HELLO nonce 匹配，拒绝按角色操作")
    if session.status.role != role:
        raise RuntimeError(f"{session.port_name} 识别为 {ROLE_NAMES.get(session.status.role)}，"
                           f"此操作要求 {ROLE_NAMES[role]}")


def _result_status(frame: Frame, action: str, sequence: int) -> int:
    if frame.sequence != sequence or frame.message_type != TYPE_DIAG_PROFILE_RESULT:
        raise ValueError("Profile RESULT 序列号或消息类型不匹配")
    if len(frame.payload) != 1:
        raise ValueError(f"Profile RESULT 长度错误：{len(frame.payload)}")
    status = frame.payload[0]
    print(f"[{action}] RESULT={status}: {INJECTION_RESULT_NAMES.get(status, 'unknown status')}")
    return status


def request_result(sessions: list[PortSession], session: PortSession, message_type: int,
                   payload: bytes, action: str, timeout_s: float) -> int:
    sequence = session.send_frame(message_type, payload)
    frame = wait_for_frame(sessions, session, sequence, (TYPE_DIAG_PROFILE_RESULT,), timeout_s)
    if frame is None:
        raise TimeoutError(f"{action}: 等待 RESULT 超时（seq={sequence}）")
    return _result_status(frame, action, sequence)


def save_profile(sessions: list[PortSession], session: PortSession, output_path: Path,
                 timeout_s: float) -> dict[str, Any]:
    ensure_role(session, 2)
    blob = bytearray()
    offset = 0
    total_expected: Optional[int] = None
    while total_expected is None or offset < total_expected:
        sequence = session.send_frame(TYPE_DIAG_PROFILE_READ, struct.pack("<I", offset))
        frame = wait_for_frame(sessions, session, sequence,
                               (TYPE_DIAG_PROFILE_DATA, TYPE_DIAG_PROFILE_RESULT), timeout_s)
        if frame is None:
            raise TimeoutError(f"Profile DATA 超时：offset={offset} seq={sequence}")
        if frame.message_type == TYPE_DIAG_PROFILE_RESULT:
            status = _result_status(frame, "Profile读取", sequence)
            raise RuntimeError(f"Profile 读取被拒绝：RESULT={status}")
        if len(frame.payload) < 8:
            raise ValueError(f"Profile DATA header 短于 8 字节：offset={offset}")
        response_offset, total = struct.unpack_from("<II", frame.payload, 0)
        data = frame.payload[8:]
        if total == 0:
            raise RuntimeError("M 当前没有可导出的完整 Profile")
        if total > PROFILE_MAX_BYTES:
            raise ValueError(f"M 上报 Profile 超过固件上限：{total} bytes")
        if total_expected is None:
            total_expected = total
        elif total != total_expected:
            raise ValueError(f"Profile 总长度在读取期间变化：{total_expected} -> {total}")
        if response_offset != offset:
            raise ValueError(f"Profile DATA offset 错误：请求 {offset}，收到 {response_offset}")
        if len(data) > PROFILE_READ_CHUNK:
            raise ValueError(f"Profile DATA chunk 超长：{len(data)}")
        if not data and offset < total_expected:
            raise ValueError(f"Profile DATA 提前空块：offset={offset}, total={total_expected}")
        blob.extend(data)
        offset += len(data)
        if len(blob) > total_expected:
            raise ValueError("Profile DATA 超过声明总长度")
    if total_expected is None or len(blob) != total_expected:
        raise ValueError(f"Profile 长度不完整：{len(blob)}/{total_expected}")

    summary: Optional[dict[str, Any]]
    try:
        summary = parse_profile_blob(bytes(blob))
    except ValueError as error:
        summary = None
        print(f"[Profile] 已完整保存传输字节，但本地 Profile v2 摘要解析失败：{error}",
              file=sys.stderr)
    crc32 = zlib.crc32(blob) & 0xFFFFFFFF
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(output_path.name + ".tmp")
    temporary_path.write_bytes(blob)
    temporary_path.replace(output_path)
    sidecar_path = Path(str(output_path) + ".json")
    sidecar = {
        "source_port": session.port_name,
        "source_role": "MOUSE_HOST",
        "saved_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "profile_file": str(output_path),
        "length": len(blob),
        "crc32": f"{crc32:08X}",
        "profile_summary": summary,
        "identity_snapshot": session.status.current_mouse,
    }
    sidecar_path.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    print(f"[Profile] 已保存 {len(blob)} bytes，CRC32={crc32:08X} -> {output_path}")
    print(f"[Profile] 本地摘要 -> {sidecar_path}")
    action = {"kind": "save_profile", "path": str(output_path), "sidecar": str(sidecar_path),
              "length": len(blob), "crc32": f"{crc32:08X}", "summary": summary}
    session.action_info = action
    session.write_json({"record_type": "profile_saved", **action})
    return action


def inject_profile(sessions: list[PortSession], session: PortSession, input_path: Path,
                   timeout_s: float) -> dict[str, Any]:
    ensure_role(session, 1)
    blob = input_path.read_bytes()
    if not blob or len(blob) > PROFILE_MAX_BYTES:
        raise ValueError(f"Profile 文件长度必须在 1..{PROFILE_MAX_BYTES}，实际 {len(blob)}")
    summary = parse_profile_blob(blob)
    crc32 = zlib.crc32(blob) & 0xFFFFFFFF
    print(f"[注入] Profile={input_path} length={len(blob)} CRC32={crc32:08X} "
          f"VID:PID={summary.get('vid_pid') or 'unknown'}")

    status = request_result(sessions, session, TYPE_DIAG_PROFILE_BEGIN,
                            struct.pack("<II", len(blob), crc32), "BEGIN", timeout_s)
    if status != 0:
        raise RuntimeError(f"Profile BEGIN 未受理：RESULT={status}")
    offset = 0
    while offset < len(blob):
        chunk = blob[offset:offset + PROFILE_WRITE_CHUNK]
        status = request_result(sessions, session, TYPE_DIAG_PROFILE_CHUNK,
                                struct.pack("<I", offset) + chunk,
                                f"CHUNK offset={offset}", timeout_s)
        if status != 0:
            raise RuntimeError(f"Profile CHUNK offset={offset} 失败：RESULT={status}")
        offset += len(chunk)
    commit_started = time.monotonic()
    status = request_result(sessions, session, TYPE_DIAG_PROFILE_COMMIT, b"", "COMMIT", timeout_s)
    if status != 0:
        raise RuntimeError(f"Profile COMMIT 未排队：RESULT={status}")
    action = {
        "kind": "inject_profile",
        "path": str(input_path),
        "length": len(blob),
        "crc32": f"{crc32:08X}",
        "summary": summary,
        "commit_result": status,
        "commit_result_meaning": "仅表示 P 接受并排队重配置，不代表 USB 已挂载",
        "commit_started_monotonic": commit_started,
    }
    session.action_info = action
    session.write_json({"record_type": "profile_injected", **{
        key: value for key, value in action.items() if key != "commit_started_monotonic"}})
    print("[注入] RESULT=0 只表示重配置已排队；继续观察 P 的“动态USB严格克隆已启用”日志。")
    return action


def print_status(sessions: list[PortSession], phase: str) -> None:
    print(f"\n[状态 {phase}]")
    for session in sessions:
        status = session.status
        role = ROLE_NAMES.get(status.role, "UNKNOWN")
        verified = "verified" if status.role_verified else "inferred/unverified"
        mouse = status.current_mouse.get("vid_pid", "unknown") if status.current_mouse else "unknown"
        copied = status.copied_profile.get("vid_pid", "unknown") \
            if status.copied_profile else "unknown"
        clone = status.clone.get("vid_pid", "unknown") if status.clone else "unknown"
        uart0 = _short_fields(status.uart0_stats, ("接收", "接受", "拒绝", "MouseReport"))
        uart1 = _short_fields(status.uart1_stats, ("tx", "rx", "peer", "drop", "vendor_drop"))
        hid = _short_fields(status.hid_stats, ("reports", "vendor_reports", "input_fail",
                                               "vendor_rx", "vendor_dropped", "failed"))
        queues = _short_queue_metrics(status.queue_metrics)
        snapshot = session.snapshot_info
        snapshot_label = (f"log={snapshot.get('status')}:{snapshot.get('bytes_received')}/"
                          f"{snapshot.get('requested_tail_bytes')}B"
                          if snapshot else "log=not-read")
        print(f"  {session.port_name:<8} {role:<12} {verified:<18} mouse={mouse} "
              f"P-profile={copied} clone={clone} {snapshot_label}")
        if uart0:
            print(f"             UART0 {uart0}")
        if uart1:
            print(f"             UART1 {uart1}")
        if hid:
            print(f"             HID   {hid}")
        if queues:
            print(f"             QUEUE {queues}")
        if session.diag_stream_status:
            diag = session.diag_stream_status
            print(f"             DIAG captured={diag['captured']} dropped={diag['dropped']} "
                  f"events={session.diag_reassembler.completed} "
                  f"incomplete={len(session.diag_reassembler.pending)}")
        if status.latest_vendor_line:
            print(f"             vendor-log {status.latest_vendor_line['text'][:180]}")


def _short_fields(stats: Optional[dict[str, Any]], names: Iterable[str]) -> str:
    if not stats:
        return ""
    fields = stats.get("fields", {})
    parts = [f"{name}={fields[name]}" for name in names if name in fields]
    return " ".join(parts)


def _short_queue_metrics(metrics: dict[str, dict[str, Any]]) -> str:
    summaries = []
    for name, values in sorted(metrics.items()):
        if "consumed" in values:
            summaries.append(
                f"{name}=consumed:{values['consumed']},overflow:{values.get('overflow', 0)},"
                f"drop:{values.get('dropped', 0)},peak:{values.get('peak', 0)}")
        else:
            summaries.append(
                f"{name}=rx:{values.get('received', 0)},rej:{values.get('rejected', 0)},"
                f"drop:{values.get('dropped', 0)},peak:{values.get('peak', 0)}")
    return " ".join(summaries)


def parse_ports(value: str) -> list[str]:
    ports = [item.strip() for item in value.split(",") if item.strip()]
    if not 1 <= len(ports) <= 2:
        raise argparse.ArgumentTypeError("--ports 要求 1 或 2 个逗号分隔的 UART0 COM 端口")
    if len(set(port.lower() for port in ports)) != len(ports):
        raise argparse.ArgumentTypeError("--ports 中不能重复同一个端口")
    return ports


def write_summaries(sessions: list[PortSession], output_directory: Path,
                    tag: str = "") -> list[Path]:
    paths = []
    suffix = f"-{tag}" if tag else ""
    for session in sessions:
        path = output_directory / f"{sanitize_filename(session.port_name)}{suffix}.summary.json"
        path.write_text(json.dumps(session.summary(), ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
        paths.append(path)
    return paths


def make_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="实时查看双板 UART0 状态与诊断事件，保存串口/事件记录，并按需备份或注入 Profile",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog=(
            "示例：\n"
            "  python tools/dual_uart_inspect.py --ports COM3,COM14 --seconds 60\n"
            "  python tools/dual_uart_inspect.py --ports COM3,COM14 --save-profile mouse.hidp\n"
            "  python tools/dual_uart_inspect.py --ports COM14 --inject-profile mouse.hidp\n"
            "  python tools/dual_uart_inspect.py --ports COM3,COM13 --inject-move 20 0\n"
            "  python tools/dual_uart_inspect.py --ports COM13 --inject-frame p 0x2A 010000000000\n"
            "--save-profile 只从已识别的 MOUSE_HOST 读取 Profile；--inject-profile 只向已识别的 "
            "PC_DEVICE 分块写 Profile。\n"
            "实时采集阶段会订阅两块板的 DIAG_STREAM_EVENT；每个 status-interval 秒发送 CONTROL=1 "
            "读取 DIAG_STREAM_STATUS，退出时发送 CONTROL=0。\n"
            "RESULT=0 仅表示重配置已排队；只有看到后续克隆成功挂载日志才能确认 USB 克隆完成。"
        ),
    )
    parser.add_argument("--ports", required=True, type=parse_ports,
                        help="一个或两个 UART0 COM 端口，例 COM3,COM14")
    parser.add_argument("--baud", type=int, default=BAUD_DEFAULT)
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="完成启动诊断和可选 Profile 操作后，继续实时采集的秒数")
    parser.add_argument("--identity-timeout", type=float, default=3.0,
                        help="等待匹配 DEVICE_HELLO nonce 的最长秒数")
    parser.add_argument("--request-timeout", type=float, default=4.0,
                        help="单个 UART0 诊断请求的响应超时秒数")
    parser.add_argument("--bootstrap-tail-bytes", type=int,
                        default=DEFAULT_BOOTSTRAP_TAIL_BYTES,
                        help="启动时每块板从板载日志尾部下载的最大字节数；0 表示跳过")
    parser.add_argument("--save-profile", type=Path,
                        help="从已确认角色为 MOUSE_HOST 的串口读取当前 Profile 并保存原始 HIDP 二进制")
    parser.add_argument("--inject-profile", type=Path,
                        help="将 Profile v2 文件显式分块写入已确认角色为 PC_DEVICE 的串口")
    parser.add_argument("--restore-auto-profile", action="store_true",
                        help="让 P 退出手动 Profile 模式，清理当前克隆并向 M 重新申请 Profile")
    parser.add_argument("--inject-move", nargs=2, metavar=("X", "Y"), type=int,
                        help="经 M UART0 注入一次无按键相对移动，并实时观察 M/P 通路")
    parser.add_argument("--inject-frame", nargs=3, metavar=("ROUTE", "TYPE", "PAYLOAD_HEX"),
                        help="显式注入已支持的载荷；ROUTE=p 表示送入 P 的虚拟 USB 输入，m 表示送入 M 的物理鼠标控制队列")
    parser.add_argument("--force-profile-refresh", action="store_true",
                        help="让已识别的 MOUSE_HOST 重新采集 Profile 并重新提议克隆"
                             "（按需复现恢复流程里的「M 重新提议 → P 复用/重装」一段）")
    parser.add_argument("--outdir", type=Path, default=Path("artifacts/tests/dual-uart-inspect"))
    parser.add_argument("--tag", default="", help="采集文件名后缀")
    parser.add_argument("--status-interval", type=float, default=1.0,
                        help="实时摘要打印和 DIAG_STREAM_STATUS 轮询间隔秒数")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    # 板载日志可能包含损坏的 UTF-8；Windows 的 GBK 控制台不能编码替换字符。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")
    parser = make_arg_parser()
    args = parser.parse_args(argv)
    if args.seconds < 0 or args.identity_timeout <= 0 or args.request_timeout <= 0:
        parser.error("--seconds 不能为负；超时必须大于 0")
    if args.status_interval <= 0:
        parser.error("--status-interval 必须大于 0")
    if args.bootstrap_tail_bytes < 0:
        parser.error("--bootstrap-tail-bytes 不能为负")
    actions = (bool(args.save_profile), bool(args.inject_profile),
               bool(args.restore_auto_profile), args.inject_move is not None,
               args.inject_frame is not None, bool(args.force_profile_refresh))
    if sum(actions) > 1:
        parser.error("Profile 备份、写入、恢复自动模式和移动探针一次只能执行一种")
    try:
        move_payload = mouse_move_payload(*args.inject_move) if args.inject_move else None
    except ValueError as error:
        parser.error(str(error))
    injected_frame = None
    if args.inject_frame:
        route_text, type_text, hex_text = args.inject_frame
        if route_text not in ("p", "m"):
            parser.error("--inject-frame ROUTE 只能为 p 或 m")
        try:
            frame_type = int(type_text, 0)
            frame_data = bytes.fromhex(hex_text)
        except ValueError:
            parser.error("--inject-frame TYPE/PAYLOAD_HEX 格式无效")
        if not 0 <= frame_type <= 255 or not 1 <= len(frame_data) <= 62:
            parser.error("--inject-frame 要求 TYPE 为 u8，载荷为 1..62 字节")
        injected_frame = ((1 if route_text == "p" else 2), frame_type, frame_data)

    start = time.monotonic()
    sessions: list[PortSession] = []
    args.outdir.mkdir(parents=True, exist_ok=True)
    try:
        for port_name in args.ports:
            session = PortSession(port_name, args.outdir, start, args.tag)
            sessions.append(session)
            try:
                session.port = open_serial_port(port_name, args.baud)
            except Exception:
                session.close()
                raise
            print(f"[打开] {port_name} @ {args.baud} baud；DTR=RTS=False；未清空接收缓冲")

        identify_boards(sessions, args.identity_timeout)

        if args.bootstrap_tail_bytes:
            print(f"[板载日志] 每端口读取最新 {args.bootstrap_tail_bytes} bytes，"
                  "状态标记为日志观察值，可能因截取/轮转而过期。")
            for session in sessions:
                snapshot = bootstrap_recent_logs(sessions, session,
                                                 args.bootstrap_tail_bytes,
                                                 args.request_timeout)
                print(f"[板载日志] {session.port_name}: {snapshot['status']} "
                      f"{snapshot.get('bytes_received', 0)}/{snapshot.get('requested_tail_bytes')} bytes; "
                      f"older_omitted={snapshot.get('older_bytes_omitted', 'unknown')}")

        set_diag_stream(sessions, True)
        # Drain the immediate subscription acknowledgements before optional Profile operations.
        ack_deadline = time.monotonic() + 0.1
        while time.monotonic() < ack_deadline:
            pump_sessions(sessions)
            time.sleep(0.002)

        print_status(sessions, "startup")

        action_session: Optional[PortSession] = None
        action_started: Optional[float] = None
        if args.save_profile:
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == 2]
            if len(candidates) != 1:
                raise RuntimeError("--save-profile 需要且只需要一个通过探测识别的 MOUSE_HOST UART0")
            action_session = candidates[0]
            action_started = time.monotonic()
            save_profile(sessions, action_session, args.save_profile, args.request_timeout)
        elif args.inject_profile:
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == 1]
            if len(candidates) != 1:
                raise RuntimeError("--inject-profile 需要且只需要一个通过探测识别的 PC_DEVICE UART0")
            action_session = candidates[0]
            action_started = time.monotonic()
            inject_profile(sessions, action_session, args.inject_profile, args.request_timeout)
        elif args.restore_auto_profile:
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == 1]
            if len(candidates) != 1:
                raise RuntimeError("--restore-auto-profile 需要且只需要一个已识别的 PC_DEVICE UART0")
            action_session = candidates[0]
            sequence = action_session.send_frame(TYPE_DIAG_PROFILE_MODE, b"\x00")
            response = wait_for_frame(sessions, action_session, sequence,
                                      (TYPE_DIAG_PROFILE_RESULT,), args.request_timeout)
            if response is None or response.payload != b"\x00":
                raise RuntimeError("P 未确认退出手动 Profile 模式")
            print("[Profile] P 已受理退出手动模式；继续观察重新克隆结果。")
        elif move_payload is not None:
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == 2]
            if len(candidates) != 1:
                raise RuntimeError("--inject-move 需要且只需要一个已识别的 MOUSE_HOST UART0")
            action_session = candidates[0]
            stages = run_move_probe(sessions, action_session, move_payload)
            action_session.action_info = {"kind": "move_probe", "payload_hex": move_payload.hex(),
                                          "observed_stages": stages}
        elif args.force_profile_refresh:
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == 2]
            if len(candidates) != 1:
                raise RuntimeError("--force-profile-refresh 需要且只需要一个已识别的 MOUSE_HOST UART0")
            action_session = candidates[0]
            sequence = action_session.send_frame(TYPE_DIAG_PROFILE_REFRESH_REQUEST, b"\x00")
            response = wait_for_frame(sessions, action_session, sequence,
                                      (TYPE_DIAG_PROFILE_RESULT,), args.request_timeout)
            if response is None or len(response.payload) != 1:
                raise RuntimeError("强制Profile重采集的结果超时或格式无效")
            action_session.action_info = {"kind": "force_profile_refresh",
                                          "dispatch_status": response.payload[0]}
            if response.payload[0] != 0:
                raise RuntimeError(f"固件拒绝强制重采集：status={response.payload[0]}")
            print("[重采集] 固件已受理：M 将重新采集 Profile 并重新提议克隆。")
        elif injected_frame is not None:
            route, frame_type, frame_data = injected_frame
            role = 1 if route == 1 else 2
            candidates = [s for s in sessions if s.status.role_verified and s.status.role == role]
            if len(candidates) != 1:
                raise RuntimeError("--inject-frame 未找到唯一匹配的目标板角色")
            action_session = candidates[0]
            payload = bytes((route, frame_type)) + frame_data
            sequence = action_session.send_frame(TYPE_DIAG_INJECT_REQUEST, payload)
            response = wait_for_frame(sessions, action_session, sequence,
                                      (TYPE_DIAG_INJECT_RESULT,), args.request_timeout)
            if response is None or len(response.payload) != 2 or response.payload[0] != route:
                raise RuntimeError("注入结果超时或格式无效")
            action_session.action_info = {"kind": "frame_injection", "route": route,
                                          "type": frame_type, "payload_hex": frame_data.hex(),
                                          "dispatch_status": response.payload[1]}
            if response.payload[1] != 0:
                raise RuntimeError(f"固件拒绝注入：status={response.payload[1]}")
            print("[帧注入] 固件已受理分发；后续结果以实时事件与 USB 完成回调为准。")

        duration = args.seconds
        deadline = time.monotonic() + duration
        next_status = time.monotonic()
        next_diag_poll = time.monotonic() + args.status_interval
        if duration == 0:
            print("[采集] --seconds=0：跳过实时等待，写出启动摘要。")
        else:
            print(f"[采集] 实时读取 {duration:.1f} 秒；Ctrl+C 可提前结束并保存已采集结果。")
            while time.monotonic() < deadline:
                pump_sessions(sessions)
                now = time.monotonic()
                if now >= next_diag_poll:
                    set_diag_stream(sessions, True)
                    next_diag_poll = now + args.status_interval
                if now >= next_status:
                    print_status(sessions, "live")
                    next_status = now + args.status_interval
                time.sleep(0.002)

        if args.inject_profile and action_session is not None and action_started is not None:
            outcome = action_session.status.clone_outcome
            outcome_after_commit = action_session.status.clone_outcome_monotonic is not None and \
                action_session.status.clone_outcome_monotonic >= \
                action_session.action_info["commit_started_monotonic"]
            action_session.action_info["post_injection_observation"] = (
                "mount_log_seen" if outcome == "mount_log_seen" else
                "failure_log_seen" if outcome == "failure_log_seen" and outcome_after_commit else
                "no_mount_outcome_log_observed"
            ) if outcome_after_commit else "no_mount_outcome_log_observed"
            if outcome == "mount_log_seen" and outcome_after_commit:
                print("[注入验收] 观察到后续“动态USB严格克隆已启用”日志。")
            elif outcome == "failure_log_seen" and outcome_after_commit:
                print("[注入验收] 观察到克隆失败日志；该次注入未确认挂载。", file=sys.stderr)
            else:
                print("[注入验收] 未观察到后续成功/失败挂载日志；RESULT=0 不代表 USB 已挂载。",
                      file=sys.stderr)

        print_status(sessions, "final")
        return 0
    except KeyboardInterrupt:
        print("\n[采集] 用户中断；保存当前已收到的数据。")
        return 130
    except (OSError, RuntimeError, TimeoutError, ValueError, struct.error) as error:
        print(f"[失败] {error}", file=sys.stderr)
        return 2
    finally:
        if any(session.diag_subscribed for session in sessions):
            try:
                set_diag_stream(sessions, False)
                # 板端可能已经开始发送多片事件；退订后仍读取一段时间，
                # 使 UART 驱动中在途的末片进入本地文件。
                drain_deadline = time.monotonic() + 0.6
                while time.monotonic() < drain_deadline:
                    pump_sessions(sessions)
                    time.sleep(0.002)
            except Exception as error:
                print(f"[DIAG_STREAM] 停止订阅失败：{error}", file=sys.stderr)
        for session in sessions:
            session.close()
        if sessions:
            for summary_path in write_summaries(sessions, args.outdir, args.tag):
                print(f"[结果] summary -> {summary_path}")
            print("[限制] UART0 原始文件只包含主机串口驱动交付的字节；驱动/硬件漏字节无法由本工具证明。")
            print("[限制] DIAG_STREAM 仅覆盖固件已接入的事件点；dropped>0 或未完成分片表示采集不完整。")


if __name__ == "__main__":
    raise SystemExit(main())
