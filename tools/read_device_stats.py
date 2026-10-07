#!/usr/bin/env python3
"""一次性读取指定双板固件（firmware/，工程名 dual_s3_hid_proxy）板卡的 UART0 内存计数和队列快照。"""

from __future__ import annotations

import argparse
import datetime
import json
import secrets
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serial  # noqa: E402

from uart_protocol import FrameScanner, build_frame  # noqa: E402

BAUD_DEFAULT = 921600
STATS_REQUEST = 0x1D
STATS_RESPONSE = 0x1E
SCHEMA_VERSION = 1
HEADER_LENGTH = 12
COUNTER_RECORD_LENGTH = 10
QUEUE_RECORD_LENGTH = 20
UNKNOWN_U16 = 0xFFFF
UNKNOWN_U32 = 0xFFFFFFFF

COUNTER_NAMES = {
    1: "reports", 2: "vendor_reports", 3: "input_fail", 4: "control",
    5: "control_fail", 6: "ctrl_retry", 7: "urb_sub", 8: "urb_ok",
    9: "urb_to", 10: "urb_retry", 11: "recover", 12: "port_cycle",
    13: "wheel", 14: "ctrl_lat_max_us", 15: "slow10", 16: "slow100",
    17: "vmin_gap_us", 18: "errors", 19: "motion_rx_dx", 20: "motion_rx_dy",
    21: "motion_rx_ok", 22: "motion_rx_badlen", 23: "motion_rx_badparse",
    24: "hb_ok", 25: "hb_fail", 26: "cycle_req", 27: "cycle_attempt",
    28: "cycle_ok", 29: "cycle_fail", 30: "cycle_off_fail", 31: "cycle_on_fail",
    32: "cycle_suppressed", 33: "input_restored", 34: "input_missing",
    35: "late500", 36: "detect_max_us", 37: "not_mounted", 38: "not_ready",
    39: "attempt", 40: "submitted", 41: "failed", 42: "complete",
    43: "transfer_fail", 44: "physical_rx", 45: "vendor_rx",
    46: "vendor_submitted", 47: "vendor_dropped", 48: "get_timeouts",
    49: "uart1_tx", 50: "uart1_rx", 51: "uart1_rx_bytes", 52: "uart1_frame_err",
    53: "uart1_rx_overflow", 54: "uart1_rx_pending_peak_bytes",
    55: "uart1_heartbeat_gap_peak_ms", 56: "uart1_raw_tx_latency_peak_us",
    57: "uart1_tx_write_fail", 58: "uart1_vendor_dropped", 59: "uart1_motion_dropped",
    60: "uart1_profile_fail", 61: "uart1_gone_retry", 62: "uart1_gone_fail",
    63: "uart1_budget_exhausted", 64: "uart_fifo_ovf_events",
    65: "uart_buffer_full_events", 66: "uart_event_reset_dropped",
    67: "p_usb_state", 68: "m_vendor_session_state",
}

QUEUE_NAMES = {
    1: "M.host_hid_event", 2: "M.host_hid_report", 3: "M.host_hid_control",
    4: "UART1.tx", 5: "UART1.motion_tx", 6: "UART1.safety_tx",
    7: "UART1.software_tx", 8: "UART1.vendor_tx", 9: "UART1.event",
    10: "P.vendor_input", 11: "P.motion_input", 12: "P.vendor_control",
    13: "UART1.rx_ring",
}


class SnapshotCollector:
    def __init__(self) -> None:
        self.role: int | None = None
        self.uptime_ms: int | None = None
        self.page_counts: dict[int, int] = {}
        self.pages: dict[int, set[int]] = {1: set(), 2: set()}
        self.counters: dict[str, int] = {}
        self.queues: dict[int, dict] = {}

    @property
    def complete(self) -> bool:
        return all(kind in self.page_counts and len(self.pages[kind]) == self.page_counts[kind]
                   for kind in (1, 2))

    def add(self, payload: bytes) -> None:
        if len(payload) < HEADER_LENGTH:
            raise ValueError("统计响应头长度不足")
        schema, role, kind, page_index, page_count, status, item_count, reserved = payload[:8]
        uptime_ms = struct.unpack_from("<I", payload, 8)[0]
        if schema != SCHEMA_VERSION or role not in (1, 2) or kind not in (1, 2) or \
                page_count == 0 or page_index >= page_count or reserved != 0:
            raise ValueError("统计响应 schema/role/kind/page/reserved 非法")
        if status != 0:
            raise ValueError(f"板端拒绝统计快照，status={status}")
        record_length, max_items = (COUNTER_RECORD_LENGTH, 5) if kind == 1 else (QUEUE_RECORD_LENGTH, 2)
        if item_count == 0 or item_count > max_items or len(payload) != HEADER_LENGTH + item_count * record_length:
            raise ValueError("统计记录数量与 payload 长度不匹配")
        if self.role is None:
            self.role, self.uptime_ms = role, uptime_ms
        elif self.role != role or self.uptime_ms != uptime_ms:
            raise ValueError("分页来自不同角色或不同 uptime 快照")
        if kind in self.page_counts and self.page_counts[kind] != page_count:
            raise ValueError("同组 page_count 不一致")
        self.page_counts[kind] = page_count
        if page_index in self.pages[kind]:
            raise ValueError(f"重复统计页 kind={kind}, index={page_index}")
        self.pages[kind].add(page_index)

        for index in range(item_count):
            start = HEADER_LENGTH + index * record_length
            record = payload[start:start + record_length]
            item_id = record[0]
            if kind == 1:
                value_type = record[1]
                if value_type == 1:
                    value = struct.unpack_from("<Q", record, 2)[0]
                elif value_type == 2:
                    value = struct.unpack_from("<q", record, 2)[0]
                else:
                    raise ValueError(f"未知计数值类型 {value_type}")
                name = COUNTER_NAMES.get(item_id, f"counter_{item_id}")
                if name in self.counters:
                    raise ValueError(f"重复计数器 id={item_id}")
                self.counters[name] = value
            else:
                unit = record[1]
                if unit not in (1, 2):
                    raise ValueError(f"未知队列单位 {unit}")
                capacity, depth, peak = struct.unpack_from("<HHH", record, 2)
                received, rejected, dropped = struct.unpack_from("<III", record, 8)
                if item_id in self.queues:
                    raise ValueError(f"重复队列 id={item_id}")
                self.queues[item_id] = {
                    "id": item_id,
                    "name": QUEUE_NAMES.get(item_id, f"queue_{item_id}"),
                    "unit": "items" if unit == 1 else "bytes",
                    "capacity": None if capacity == UNKNOWN_U16 else capacity,
                    "depth": None if depth == UNKNOWN_U16 else depth,
                    "peak": None if peak == UNKNOWN_U16 else peak,
                    "received": None if received == UNKNOWN_U32 else received,
                    "rejected": None if rejected == UNKNOWN_U32 else rejected,
                    "dropped": None if dropped == UNKNOWN_U32 else dropped,
                }

    def result(self) -> dict:
        if not self.complete or self.role is None or self.uptime_ms is None:
            raise ValueError("统计快照缺页")
        common_counters = set(range(49, 67))
        if self.role == 2:
            required_counters = set(range(1, 37)) | common_counters | {68}
            required_queues = set(range(1, 10)) | {13}
            role_name = "M/MOUSE_HOST"
        else:
            required_counters = set(range(37, 49)) | common_counters | {67}
            required_queues = set(range(4, 10)) | {10, 11, 12, 13}
            role_name = "P/PC_DEVICE"
        # Check by names, keeping the public JSON counter keys human-readable.
        missing = [counter_id for counter_id in required_counters
                   if COUNTER_NAMES[counter_id] not in self.counters]
        if missing:
            raise ValueError(f"缺少必需计数器 id={missing}")
        missing_queues = required_queues - self.queues.keys()
        if missing_queues:
            raise ValueError(f"缺少必需队列 id={sorted(missing_queues)}")
        state_counter_name = COUNTER_NAMES[67 if self.role == 1 else 68]
        packed = self.counters[state_counter_name]
        if self.role == 1:
            result_id = (packed >> 8) & 0xFF
            result_names = {
                0: "unknown", 1: "wait_host", 2: "final_ack_pending",
                3: "mounted_acked", 4: "install_failed",
                5: "final_ack_failed", 6: "canceled",
            }
            state = {
                "attached": bool(packed & (1 << 0)),
                "installed": bool(packed & (1 << 1)),
                "cloneActive": bool(packed & (1 << 2)),
                "mounted": bool(packed & (1 << 3)),
                "reconfigureInProgress": bool(packed & (1 << 4)),
                "waitingHost": bool(packed & (1 << 5)),
                "finalAckFailed": bool(packed & (1 << 6)),
                "disconnectPending": bool(packed & (1 << 7)),
                "waitingFinalAck": bool(packed & (1 << 16)),
                "lastResultId": result_id,
                "lastResult": result_names.get(result_id, f"unknown_{result_id}"),
                "operationEpoch": packed >> 32,
            }
        else:
            state = {
                "vendorSessionActive": bool(packed & (1 << 0)),
                "firstVendorRequestSeen": bool(packed & (1 << 1)),
                "peerGenerationCurrent": bool(packed & (1 << 2)),
                "waitingHost": bool(packed & (1 << 3)),
                "vendorSessionEpoch": packed >> 32,
            }
        return {
            "role": role_name,
            "roleId": self.role,
            "uptimeMilliseconds": self.uptime_ms,
            "counters": self.counters,
            "state": state,
            "queues": [self.queues[key] for key in sorted(self.queues)],
        }


def main() -> int:
    parser = argparse.ArgumentParser(description="读取指定 ESP32-S3 板卡的双板固件统计快照")
    parser.add_argument("--port", required=True, help="目标板 UART0 串口，例如 COM12")
    parser.add_argument("--baud", type=int, default=BAUD_DEFAULT)
    parser.add_argument("--timeout", type=float, default=4.0, help="完整收到所有分页的超时秒数")
    args = parser.parse_args()
    if args.baud <= 0 or args.timeout <= 0:
        parser.error("--baud 与 --timeout 必须大于 0")

    port = serial.Serial(port=None, baudrate=args.baud, timeout=0.05, write_timeout=1.0,
                         rtscts=False, dsrdtr=False)
    # 先在未打开状态设低 DTR/RTS，避免 pyserial 用默认高电平触碰 ESP32 自动复位电路。
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
    except serial.SerialException as error:
        print(f"打开 {args.port} 失败：{error}", file=sys.stderr)
        return 2

    sequence = secrets.randbelow(0x10000)
    scanner = FrameScanner()
    collector = SnapshotCollector()
    try:
        request = build_frame(STATS_REQUEST, sequence, b"")
        if port.write(request) != len(request):
            raise IOError("统计请求帧未完整写入")
        port.flush()
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and not collector.complete:
            for message_type, frame_sequence, payload in scanner.feed(port.read(4096)):
                if message_type == STATS_RESPONSE and frame_sequence == sequence:
                    collector.add(payload)
        if not collector.complete:
            raise TimeoutError("未在超时内收齐 counters 与 queues 两组统计分页")
        snapshot = collector.result()
        output = {
            "readAt": datetime.datetime.now().astimezone().isoformat(timespec="milliseconds"),
            "port": args.port,
            "snapshot": snapshot,
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, TimeoutError) as error:
        print(f"统计快照读取失败：{error}", file=sys.stderr)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    raise SystemExit(main())
