#!/usr/bin/env python3
"""真实串口到 USB HID 的 20 ms 鼠标移动验收。"""

import argparse
import re
import struct
import sys
import threading
import time

import serial


MOUSE_STATS = re.compile(
    r"接收=(?P<received>\d+) 位移=\((?P<received_x>-?\d+),(?P<received_y>-?\d+)\).*"
    r"提交=(?P<submitted>\d+) 位移=\((?P<submitted_x>-?\d+),(?P<submitted_y>-?\d+)\).*"
    r"完成=(?P<completed>\d+) 位移=\((?P<completed_x>-?\d+),(?P<completed_y>-?\d+)\).*"
    r"待发送=\((?P<pending_x>-?\d+),(?P<pending_y>-?\d+)\).*"
    r"最大积压=\((?P<max_pending_x>\d+),(?P<max_pending_y>\d+)\)/(?P<max_pending_reports>\d+)帧"
)
USB_STATS = re.compile(
    r"任务tick=(?P<ticks>\d+).*mounted=(?P<mounted_true>\d+)/(?P<mounted_false>\d+).*"
    r"有效移动 接收/提交/完成=(?P<received>\d+)/(?P<submitted>\d+)/(?P<completed>\d+).*"
    r"提交失败=(?P<failed>\d+).*完成最大间隔=(?P<max_gap_us>\d+) us"
)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class HardwareCheck:
    def __init__(self, port: str, baud: int) -> None:
        self.sequence = 0
        self.stop = False
        self.lines: list[str] = []
        self.mouse_stats: list[dict[str, int]] = []
        self.usb_stats: list[dict[str, int]] = []
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baud
        self.serial.timeout = 0.05
        self.serial.write_timeout = 1
        self.serial.dtr = False
        self.serial.rts = False

    def encode(self, kind: int, payload: bytes = b"") -> bytes:
        body = bytes((2, kind)) + struct.pack("<H", self.sequence) + bytes((len(payload),)) + payload
        self.sequence = (self.sequence + 1) & 0xFFFF
        return b"\xA5\x5A" + body + struct.pack("<H", crc16(body))

    def reader(self) -> None:
        buffer = b""
        while not self.stop:
            buffer += self.serial.read(4096)
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.lines.append(line)
                mouse = MOUSE_STATS.search(line)
                if mouse:
                    self.mouse_stats.append({key: int(value) for key, value in mouse.groupdict().items()})
                    print(line, flush=True)
                usb = USB_STATS.search(line)
                if usb:
                    self.usb_stats.append({key: int(value) for key, value in usb.groupdict().items()})
                    print(line, flush=True)

    def write(self, kind: int, payload: bytes = b"") -> None:
        self.serial.write(self.encode(kind, payload))

    def wait_for_mouse_stats(self, predicate, timeout: float = 4.0) -> dict[str, int]:
        deadline = time.perf_counter() + timeout
        while time.perf_counter() < deadline:
            if self.mouse_stats and predicate(self.mouse_stats[-1]):
                return self.mouse_stats[-1]
            time.sleep(0.02)
        raise AssertionError("等待固件鼠标统计超时")

    def run_case(self, dx: int) -> None:
        baseline = self.mouse_stats[-1]
        usb_start = len(self.usb_stats)
        self.write(5)  # SessionStart，同时清空上一会话输入状态
        self.serial.flush()
        time.sleep(0.25)

        timestamps: list[float] = []
        start = time.perf_counter()
        for index in range(50):
            remaining = start + index * 0.020 - time.perf_counter()
            if remaining > 0:
                time.sleep(remaining)
            payload = bytes((0,)) + struct.pack("<hhbb", dx, 0, 0, 0)
            self.write(2, payload)
            timestamps.append(time.perf_counter())
        self.serial.flush()

        expected_x = baseline["received_x"] + dx * 50
        final = self.wait_for_mouse_stats(
            lambda stats: stats["received"] >= baseline["received"] + 50
            and stats["completed_x"] == expected_x
            and stats["pending_x"] == 0
        )
        time.sleep(0.15)
        diagnostics = self.usb_stats[usb_start:]
        intervals = [(right - left) * 1000 for left, right in zip(timestamps, timestamps[1:])]

        actual = {
            "received": final["received"] - baseline["received"],
            "submitted_x": final["submitted_x"] - baseline["submitted_x"],
            "completed_x": final["completed_x"] - baseline["completed_x"],
            "pending_x": final["pending_x"],
            "motion_received": sum(item["received"] for item in diagnostics),
            "motion_submitted": sum(item["submitted"] for item in diagnostics),
            "motion_completed": sum(item["completed"] for item in diagnostics),
            "submit_failed": sum(item["failed"] for item in diagnostics),
            "max_completion_gap_us": max((item["max_gap_us"] for item in diagnostics), default=0),
            "max_send_interval_ms": max(intervals),
        }
        expected = {
            "received": 50,
            "submitted_x": dx * 50,
            "completed_x": dx * 50,
            "pending_x": 0,
            "motion_received": 50,
            "motion_submitted": 50,
            "motion_completed": 50,
            "submit_failed": 0,
        }
        errors = [f"{key}: 期望={value}，实际={actual[key]}" for key, value in expected.items() if actual[key] != value]
        if actual["max_completion_gap_us"] > 40_000:
            errors.append(f"max_completion_gap_us: 期望<=40000，实际={actual['max_completion_gap_us']}")
        if actual["max_send_interval_ms"] > 40:
            errors.append(f"max_send_interval_ms: 期望<=40，实际={actual['max_send_interval_ms']:.3f}")

        print(f"用例 dx={dx}：期望={expected}，实际={actual}", flush=True)
        if errors:
            raise AssertionError("；".join(errors))
        time.sleep(1.8)

    def run(self) -> None:
        self.serial.open()
        thread = threading.Thread(target=self.reader, daemon=True)
        thread.start()
        try:
            self.wait_for_mouse_stats(lambda _: True)
            deadline = time.perf_counter() + 3
            while time.perf_counter() < deadline and not (
                self.usb_stats and self.usb_stats[-1]["mounted_true"] > 0
            ):
                time.sleep(0.05)
            if not self.usb_stats or self.usb_stats[-1]["mounted_true"] == 0:
                raise AssertionError("USB HID 目标未连接：请先连接手机并确认其已识别键鼠设备")
            for dx in (20, 5, 1):
                self.run_case(dx)
            if any("输入帧序号不连续" in line or "丢弃输出帧" in line for line in self.lines):
                raise AssertionError("日志包含序号不连续或输出丢弃")
            print("真实硬件 20 ms 鼠标移动检查通过", flush=True)
        finally:
            try:
                self.write(3)  # ReleaseAll
                self.serial.flush()
            finally:
                self.stop = True
                thread.join(timeout=1)
                self.serial.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="ESP32 UART 串口，例如 COM3")
    parser.add_argument("--baud", type=int, default=921600)
    args = parser.parse_args()
    try:
        HardwareCheck(args.port, args.baud).run()
        return 0
    except Exception as error:
        print(f"真实硬件 20 ms 鼠标移动检查失败：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
