#!/usr/bin/env python3
"""Run read-only MAKCU V4 mouse API checks over a serial port.

This tool only sends queries. It does not send reset, movement, click, mask,
subscription, or baud-setting commands. Close the Host application before use;
the tool never stops or changes other processes. DTR and RTS are held low before
opening the port to avoid the usual USB-UART reset wiring.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import time
from collections.abc import Callable


PROMPT = b">>> "
SYNC = b"\xDE\xAD"
MAX_PAYLOAD = 64


class CheckFailure(Exception):
    pass


def read_exact(port, count: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < count and time.monotonic() < deadline:
        chunk = port.read(count - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != count:
        raise TimeoutError(f"响应不完整：需要 {count} 字节，收到 {len(data)}")
    return bytes(data)


def read_until_prompt(port, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        byte = port.read(1)
        if byte:
            data.extend(byte)
            if data.endswith(PROMPT):
                return bytes(data)
    raise TimeoutError(f"等待 ASCII prompt 超时；收到 {bytes(data)!r}")


def ascii_query(port, command: str, timeout: float) -> str:
    wire = command.encode("ascii") + b"\r"
    port.write(wire)
    response = read_until_prompt(port, timeout)
    body = response[: -len(PROMPT)].replace(b"\r", b"")
    lines = [line.strip() for line in body.split(b"\n") if line.strip()]
    if lines and lines[0] == command.encode("ascii"):
        lines.pop(0)  # Query echo is optional; version is never echoed.
    if not lines:
        raise CheckFailure(f"{command}: 收到空结果 {response!r}")
    if lines[-1] == b"ERR":
        raise CheckFailure(f"{command}: 设备返回 ERR")
    return lines[-1].decode("ascii", errors="replace")


def read_binary_frame(port, timeout: float) -> tuple[int, bytes]:
    deadline = time.monotonic() + timeout
    matched = 0
    while time.monotonic() < deadline:
        byte = port.read(1)
        if not byte:
            continue
        value = byte[0]
        if matched == 0:
            matched = 1 if value == SYNC[0] else 0
        elif value == SYNC[1]:
            matched = 0
            header = read_exact(port, 3, deadline)
            payload_length = header[0] | (header[1] << 8)
            opcode = header[2]
            if payload_length > MAX_PAYLOAD:
                raise CheckFailure(f"二进制响应长度异常：{payload_length}")
            payload = read_exact(port, payload_length, deadline)
            return opcode, payload
        else:
            matched = 1 if value == SYNC[0] else 0
    raise TimeoutError("等待 MAK_API 二进制响应超时")


def binary_query(port, opcode: int, payload: bytes, timeout: float) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("查询载荷超过协议上限")
    frame = SYNC + struct.pack("<H", len(payload)) + bytes([opcode]) + payload
    port.write(frame)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        received_opcode, received_payload = read_binary_frame(port, remaining)
        if received_opcode == opcode:
            if received_payload == b"\xFF":
                raise CheckFailure(f"opcode 0x{opcode:02X}: 设备返回协议错误")
            return received_payload
        # Physical button stream events (0x53) may be interleaved with queries.
        if received_opcode != 0x53:
            raise CheckFailure(
                f"opcode 0x{opcode:02X}: 收到意外响应 0x{received_opcode:02X}"
            )
    raise TimeoutError(f"opcode 0x{opcode:02X}: 等待响应超时")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def ascii_checks(port, timeout: float, expected_baud: int) -> list[tuple[str, Callable[[], None]]]:
    def query(command: str) -> str:
        return ascii_query(port, command, timeout)

    checks: list[tuple[str, Callable[[], None]]] = []

    def exact(command: str, expected: str) -> None:
        actual = query(command)
        require(actual == expected, f"{command}: 期望 {expected!r}，收到 {actual!r}")

    checks.append(("ASCII version 握手", lambda: exact("km.version()", "km.MAKCU")))
    checks.append(("ASCII device 类型", lambda: exact("km.device()", "mouse")))

    def echo() -> None:
        value = query("km.echo()")
        require(value in {"0", "1"}, f"echo 值异常：{value!r}")

    checks.append(("ASCII echo 查询", echo))

    def baud() -> None:
        value = query("km.baud()")
        require(value == str(expected_baud),
                f"baud 与串口参数不一致：设备={value!r}，端口={expected_baud}")

    checks.append(("ASCII baud 查询", baud))

    def screen() -> None:
        value = query("km.screen()")
        match = re.fullmatch(r"km\.screen\((\d+),(\d+)\)", value)
        require(match is not None, f"screen 结果格式异常：{value!r}")
        width, height = (int(part) for part in match.groups())
        require(1 <= width <= 32767 and 1 <= height <= 32767,
                f"screen 范围异常：{width}x{height}")

    checks.append(("ASCII screen 查询", screen))

    def getpos() -> None:
        value = query("km.getpos()")
        match = re.fullmatch(r"km\.getpos\((\d+),(\d+)\)", value)
        require(match is not None, f"getpos 结果格式异常：{value!r}")
        x, y = (int(part) for part in match.groups())
        screen_text = query("km.screen()")
        screen_match = re.fullmatch(r"km\.screen\((\d+),(\d+)\)", screen_text)
        require(screen_match is not None, f"screen 结果格式异常：{screen_text!r}")
        width, height = (int(part) for part in screen_match.groups())
        require(x < width and y < height, f"getpos 超出屏幕：({x},{y}) / {width}x{height}")

    checks.append(("ASCII getpos 查询", getpos))

    def scalar(command: str, allowed: set[str]) -> None:
        value = query(command)
        require(value in allowed, f"{command}: 值异常 {value!r}")

    for command in ("km.left()", "km.right()", "km.middle()", "km.side1()", "km.side2()"):
        checks.append((f"ASCII {command} 查询", lambda cmd=command: scalar(cmd, {"0", "1", "2", "3"})))
    checks.append(("ASCII physical buttons 查询", lambda: scalar("km.phys_buttons()", {str(i) for i in range(32)})))
    checks.append(("ASCII moving 查询", lambda: scalar("km.moving()", {"0", "1"})))
    checks.append(("ASCII stream 查询", lambda: scalar("km.stream(mouse)", {"0", "1"})))
    checks.append(("ASCII buttons 模式查询", lambda: scalar("km.buttons()", {"0", "1", "2", "3"})))
    checks.append(("ASCII interpolate 查询", lambda: scalar("km.interpolate()", {*(str(i) for i in range(101)), "255"})))
    lock_names = (
        "lock_ml", "lock_mr", "lock_mm", "lock_ms1", "lock_ms2",
        "lock_mx", "lock_mx+", "lock_mx-", "lock_my", "lock_my+",
        "lock_my-", "lock_mw", "lock_mw+", "lock_mw-",
    )
    for name in lock_names:
        checks.append((f"ASCII {name} 查询", lambda cmd=f"km.{name}()": scalar(cmd, {"0", "1"})))
    return checks


def binary_checks(port, timeout: float, expected_baud: int) -> list[tuple[str, Callable[[], None]]]:
    def query(opcode: int, payload: bytes = b"") -> bytes:
        return binary_query(port, opcode, payload, timeout)

    checks: list[tuple[str, Callable[[], None]]] = []

    def one_byte(
        opcode: int,
        label: str,
        predicate: Callable[[int], bool],
        payload: bytes = b"",
    ) -> int:
        data = query(opcode, payload)
        require(len(data) == 1 and predicate(data[0]),
                f"{label}: 响应载荷异常 {data.hex()}")
        return data[0]

    checks.append(("binary DEVICE", lambda: require(one_byte(0x02, "DEVICE", lambda v: v == 1) == 1,
                                                     "DEVICE 类型不是 mouse")))

    def api_level() -> None:
        data = query(0x04)
        require(len(data) == 4 and struct.unpack("<I", data)[0] == 4,
                f"桥接 API 级别响应异常：{data.hex()}")

    checks.append(("binary bridge API level", api_level))

    for opcode in range(0x11, 0x16):
        def button_query(op=opcode) -> None:
            one_byte(op, f"button 0x{op:02X}", lambda v: v <= 1)
        checks.append((f"binary button 0x{opcode:02X}", button_query))

    def subscription_consistency() -> None:
        buttons = one_byte(0x10, "BUTTONS", lambda v: v <= 1)
        stream = query(0x52, b"\x01")
        require(len(stream) == 1 and stream[0] <= 1,
                f"INPUT_STREAM(mouse) 响应异常：{stream.hex()}")
        require(buttons == stream[0], f"BUTTONS={buttons} 与 INPUT_STREAM={stream[0]} 不一致")

    checks.append(("binary BUTTONS / INPUT_STREAM 状态一致", subscription_consistency))

    checks.append(("binary physical buttons", lambda: one_byte(0x55, "phys_buttons", lambda v: v <= 0x1F)))

    def snapshot() -> None:
        data = query(0x56)
        require(len(data) == 40, f"snapshot 长度应为 40，收到 {len(data)}")
        require(data[0] <= 0x1F and struct.unpack_from("<H", data, 1)[0] <= 0x3FFF,
                "snapshot 按钮或 lock mask 超出范围")
        require(data[3] == 0 and not any(data[4:36]) and not any(data[37:40]),
                "snapshot 键盘/保留字段应为零")
        require(data[36] <= 100 or data[36] == 255,
                f"snapshot 插值值异常：{data[36]}")

    checks.append(("binary 40-byte snapshot", snapshot))

    def locks() -> None:
        for target in range(14):
            value = one_byte(0x60, f"lock target {target}", lambda v: v <= 1,
                             bytes([target]))
            require(value in (0, 1), f"lock target {target} 值异常：{value}")

    checks.append(("binary 14 个 lock 查询", locks))
    checks.append(("binary interpolate", lambda: one_byte(0x1F, "interpolate", lambda v: v <= 100 or v == 255)))

    def screen() -> tuple[int, int]:
        data = query(0x64)
        require(len(data) == 4, f"SCREEN 响应长度异常：{data.hex()}")
        width, height = struct.unpack("<HH", data)
        require(1 <= width <= 32767 and 1 <= height <= 32767,
                f"SCREEN 范围异常：{width}x{height}")
        return width, height

    checks.append(("binary SCREEN", lambda: screen()))

    def pointer() -> None:
        data = query(0x63)
        require(len(data) == 4, f"GETPOS 响应长度异常：{data.hex()}")
        x, y = struct.unpack("<HH", data)
        width, height = screen()
        require(x < width and y < height, f"GETPOS 超出屏幕：({x},{y}) / {width}x{height}")

    checks.append(("binary GETPOS", pointer))

    def moving() -> None:
        one_byte(0x66, "MOVING", lambda v: v <= 1)

    checks.append(("binary MOVING", moving))

    def baud() -> None:
        data = query(0xA4)
        require(len(data) == 4 and struct.unpack("<I", data)[0] == expected_baud,
                f"GET_BAUD 与端口参数不一致：{data.hex()} / {expected_baud}")

    checks.append(("binary GET_BAUD", baud))
    return checks


def run_checks(checks: list[tuple[str, Callable[[], None]]]) -> tuple[int, int]:
    passed = failed = 0
    for name, check in checks:
        try:
            check()
            passed += 1
            print(f"[PASS] {name}")
        except Exception as exc:  # Keep running so one bad field does not hide later evidence.
            failed += 1
            print(f"[FAIL] {name}: {exc}")
    return passed, failed


def main() -> int:
    parser = argparse.ArgumentParser(
        description="只读检查 MAKCU V4 ASCII 与 MAK_API 鼠标查询；不发复位或鼠标输入。"
    )
    parser.add_argument("--port", required=True, help="M 板 USB-UART 串口，例如 COM12")
    parser.add_argument("--baud", type=int, default=115200,
                        choices=(115200, 4000000), help="M UART0 当前波特率，默认 115200")
    parser.add_argument("--timeout", type=float, default=1.5,
                        help="单条查询响应超时秒数，默认 1.5")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout 必须大于零")

    try:
        import serial  # type: ignore[import-not-found]
    except ImportError:
        print("缺少 pyserial；请运行 python -m pip install pyserial", file=sys.stderr)
        return 2

    port = serial.Serial(
        port=None,
        baudrate=args.baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.05,
        write_timeout=1.0,
        dsrdtr=False,
        rtscts=False,
    )
    port.port = args.port
    port.dtr = False
    port.rts = False
    try:
        port.open()
    except Exception as exc:
        print(f"无法打开 {args.port}：{exc}", file=sys.stderr)
        return 2

    print(f"只读查询 {args.port} @ {args.baud} 8N1；未发送复位、移动或点击命令。")
    try:
        ascii_result = run_checks(ascii_checks(port, args.timeout, args.baud))
        binary_result = run_checks(binary_checks(port, args.timeout, args.baud))
    finally:
        port.close()
    passed = ascii_result[0] + binary_result[0]
    failed = ascii_result[1] + binary_result[1]
    print(f"结果：{passed} PASS，{failed} FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
