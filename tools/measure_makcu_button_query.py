"""测量 Makcu（MAK_API V4）按键查询在本机的往返延迟。

用途：回答「sender 改用 Makcu 接口查询实体鼠标按键，一次查询要多久」。
本脚本只发只读查询与一次可选的 ``0x61 click`` 注入探针，**不做任何复位动作**：
打开串口前先把 DTR/RTS 置低，避免经过开发板自动复位电路（见 AGENTS.md
「串口查询默认不触发复位」）。

运行前必须让出 M 板 UART0：Host EXE 占用该口时无法测量。

    py -3 tools/measure_makcu_button_query.py --port COM12
    py -3 tools/measure_makcu_button_query.py --port COM12 --count 300 --stream-clicks 20

输出为终端表格 + JSON 文件（默认写入 ``artifacts/tests/makcu-button-query/``）。
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import struct
import sys
import time
from pathlib import Path

SYNC = b"\xDE\xAD"
MAX_PAYLOAD = 4096

OP_DEVICE = 0x02
OP_VERSION = 0x04
OP_BUTTONS_SUBSCRIBE = 0x10
OP_PHYS_BUTTONS = 0x55
OP_CLICK = 0x61
OP_BUTTON_EVENT = 0x53


class ProbeError(RuntimeError):
    pass


class SerialLink:
    """带同步帧扫描的串口连接；打开过程不触发板卡复位。"""

    def __init__(self, port_name: str, baud: int, timeout: float) -> None:
        import serial  # type: ignore[import-not-found]

        # 先构造、后设 port，并在 open() 之前置低 DTR/RTS：
        # serial.Serial(port, baud) 会经过默认电平，经自动复位电路复位板卡。
        self._port = serial.Serial()
        self._port.port = port_name
        self._port.baudrate = baud
        self._port.bytesize = serial.EIGHTBITS
        self._port.parity = serial.PARITY_NONE
        self._port.stopbits = serial.STOPBITS_ONE
        self._port.timeout = timeout
        self._port.write_timeout = 1.0
        self._port.dsrdtr = False
        self._port.rtscts = False
        self._port.dtr = False
        self._port.rts = False
        self._port.open()
        self._port.dtr = False
        self._port.rts = False
        self._port.reset_input_buffer()
        self._port.reset_output_buffer()
        self.port_name = port_name
        self.baud = baud

    def close(self) -> None:
        try:
            self._port.close()
        except Exception:  # noqa: BLE001 - 关闭失败不应掩盖测量结果
            pass

    def write_frame(self, opcode: int, payload: bytes = b"") -> float:
        """写入一帧并返回写完成的高精度时间戳（秒）；必须用 perf_counter，time.monotonic 在本机只有 15.6 ms 粒度。"""
        frame = SYNC + struct.pack("<H", len(payload)) + bytes([opcode]) + payload
        self._port.write(frame)
        return time.perf_counter()

    def drain(self) -> int:
        """丢弃当前已缓冲的旧字节（不发送任何命令）。"""
        waiting = self._port.in_waiting
        if waiting:
            self._port.read(waiting)
        return waiting

    def read_frame(self, deadline: float, allow_events: bool = False):
        """扫描下一个同步帧，返回 (opcode, payload, 首字节时刻, 完成时刻)。"""
        matched = 0
        first_byte_at = None
        while time.perf_counter() < deadline:
            chunk = self._port.read(1)
            if not chunk:
                continue
            value = chunk[0]
            if matched == 0:
                if value == SYNC[0]:
                    matched = 1
                continue
            if value == SYNC[1]:
                matched = 0
                header = self._read_exact(3, deadline)
                length = header[0] | (header[1] << 8)
                opcode = header[2]
                if length > MAX_PAYLOAD:
                    raise ProbeError(f"响应长度异常：{length}")
                payload = self._read_exact(length, deadline) if length else b""
                completed = time.perf_counter()
                if first_byte_at is None:
                    first_byte_at = completed
                if allow_events and opcode == OP_BUTTON_EVENT:
                    return opcode, payload, first_byte_at, completed
                if opcode == OP_BUTTON_EVENT:
                    # 订阅事件与查询响应可能交错；非 events 模式下继续找目标帧。
                    continue
                return opcode, payload, first_byte_at, completed
            matched = 1 if value == SYNC[0] else 0
        raise TimeoutError("等待 MAK_API 二进制响应超时")

    def _read_exact(self, count: int, deadline: float) -> bytes:
        data = bytearray()
        while len(data) < count and time.perf_counter() < deadline:
            chunk = self._port.read(count - len(data))
            if chunk:
                data.extend(chunk)
        if len(data) != count:
            raise TimeoutError(f"读取 {count} 字节超时（已收到 {len(data)}）")
        return bytes(data)

    def query(self, opcode: int, payload: bytes = b"", timeout: float = 1.5):
        self.write_frame(opcode, payload)
        deadline = time.perf_counter() + timeout
        while True:
            received_opcode, received_payload, first_at, done_at = self.read_frame(deadline)
            if received_opcode == opcode:
                if received_payload == b"\xFF":
                    raise ProbeError(f"opcode 0x{opcode:02X} 返回协议错误")
                return received_payload, first_at, done_at
            raise ProbeError(f"期望 0x{opcode:02X}，收到 0x{received_opcode:02X}")


def summarize(samples_us):
    ordered = sorted(samples_us)
    return {
        "count": len(ordered),
        "min": ordered[0],
        "median": statistics.median(ordered),
        "mean": statistics.fmean(ordered),
        "p95": ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))],
        "max": ordered[-1],
    }


def print_summary(title: str, stats: dict, unit: str = "us") -> None:
    print(
        f"{title}: n={stats['count']} 中位 {stats['median']:.0f} {unit} / "
        f"均值 {stats['mean']:.0f} {unit} / P95 {stats['p95']:.0f} {unit} / "
        f"最大 {stats['max']:.0f} {unit}（最小 {stats['min']:.0f} {unit}）"
    )


def measure_query_latency(link: SerialLink, count: int, gap_ms: float, timeout: float):
    """连续查询 0x55（原始物理按钮掩码），返回每次往返耗时与观察到的状态分布。

    每次查询前清空输入缓冲：否则同步扫描会立刻吃到一个陈旧帧，把往返时间记成 0。
    """
    round_trips = []
    states = {}
    for index in range(count):
        if gap_ms > 0 and index:
            time.sleep(gap_ms / 1000.0)
        link.drain()
        sent_at = link.write_frame(OP_PHYS_BUTTONS)
        deadline = time.perf_counter() + timeout
        opcode, payload, _first_at, done_at = link.read_frame(deadline)
        if opcode != OP_PHYS_BUTTONS:
            raise ProbeError(f"查询响应 opcode 异常：0x{opcode:02X}")
        if payload == b"\xFF":
            raise ProbeError("phys_buttons 返回协议错误")
        value = payload[0] if payload else None
        states[value] = states.get(value, 0) + 1
        round_trips.append((done_at - sent_at) * 1e6)
    return round_trips, states


def measure_stream_events(link: SerialLink, clicks: int, timeout: float):
    """订阅物理按钮事件（0x10），用 click 注入探针测量事件到达延迟。"""
    link.drain()
    link.write_frame(OP_BUTTONS_SUBSCRIBE, b"\x01")
    time.sleep(0.02)
    link.drain()
    press_latency = []
    release_latency = []
    payloads = []
    try:
        for _ in range(clicks):
            link.drain()
            link.write_frame(OP_CLICK, bytes([1, 1, 0x1E, 0x00]))
            sent_at = time.perf_counter()
            deadline = sent_at + timeout
            # click(1,1,hold=30ms) 先产生「按下」事件，再产生「松开」事件。
            events = 0
            while events < 2 and time.perf_counter() < deadline:
                opcode, payload, byte_at, _done_at = link.read_frame(deadline, allow_events=True)
                if opcode != OP_BUTTON_EVENT:
                    continue
                events += 1
                payloads.append(payload.hex())
                latency = (byte_at - sent_at) * 1e6
                if events == 1:
                    press_latency.append(latency)
                else:
                    release_latency.append(latency)
            if events < 2:
                raise ProbeError("按钮事件不足（订阅可能因队列溢出被关闭）")
            time.sleep(0.05)
    finally:
        # 0x10 的设置不回包，只能发完自行清缓冲。
        link.drain()
        link.write_frame(OP_BUTTONS_SUBSCRIBE, b"\x00")
        time.sleep(0.02)
        link.drain()
    return press_latency, release_latency, payloads


def detect_makcu_port(baud: int, timeout: float):
    """在可用串口中探测哪一个是 M 板（DEVICE=0x01）。只发只读查询。"""
    import serial  # type: ignore[import-not-found]

    from serial.tools import list_ports  # type: ignore[import-not-found]

    candidates = [item.device for item in list_ports.comports()]
    print(f"候选串口：{candidates}")
    usable = []
    for candidate in candidates:
        try:
            link = SerialLink(candidate, baud, timeout=0.05)
        except Exception as exc:  # noqa: BLE001 - 被占用是常见情况
            print(f"  {candidate}: 打不开（{exc}）")
            continue
        try:
            device, _, _ = link.query(OP_DEVICE, timeout=timeout)
            level, _, _ = link.query(OP_VERSION, timeout=timeout)
            print(f"  {candidate}: DEVICE={device.hex()} API level={int.from_bytes(level, 'little')}")
            if device[:1] == b"\x01":
                usable.append(candidate)
        except Exception as exc:  # noqa: BLE001 - 非 Makcu 端口的正常结果
            print(f"  {candidate}: 无 MAK_API 响应（{type(exc).__name__}）")
        finally:
            link.close()
    return usable


def main() -> int:
    parser = argparse.ArgumentParser(
        description="测量 Makcu（MAK_API V4）按键查询往返延迟；只读查询 + 可选 click 注入探针，不复位板卡。"
    )
    parser.add_argument("--port", help="M 板 USB-UART 串口，例如 COM12")
    parser.add_argument("--detect", action="store_true", help="探测所有串口并报告哪个是 M 板（只读查询）")
    parser.add_argument("--baud", type=int, default=115200, choices=(115200, 4000000),
                        help="M UART0 当前波特率，默认 115200（4M 未验证）")
    parser.add_argument("--count", type=int, default=200, help="0x55 查询样本数，默认 200")
    parser.add_argument("--gap-ms", type=float, default=0.0,
                        help="查询之间的间隔毫秒数，默认 0（背靠背压力口径）")
    parser.add_argument("--stream-clicks", type=int, default=10,
                        help="事件订阅探针的 click 次数，0 表示跳过，默认 10")
    parser.add_argument("--timeout", type=float, default=1.5, help="单次响应超时秒数，默认 1.5")
    parser.add_argument("--output", type=Path, default=None, help="JSON 输出路径")
    args = parser.parse_args()

    try:
        import serial  # type: ignore[import-not-found]  # noqa: F401
    except ImportError:
        print("缺少 pyserial；请运行 python -m pip install pyserial", file=sys.stderr)
        return 2

    if not args.port:
        if not args.detect:
            parser.error("需要 --port，或用 --detect 先探测端口")
        found = detect_makcu_port(args.baud, args.timeout)
        print(f"M 板候选端口：{found}")
        if len(found) != 1:
            print("请用 --port 明确指定 M 板串口后重跑。", file=sys.stderr)
            return 2
        args.port = found[0]

    try:
        link = SerialLink(args.port, args.baud, timeout=0.05)
    except Exception as exc:  # noqa: BLE001 - 端口被占用是最常见的失败原因
        print(f"无法打开 {args.port}：{exc}", file=sys.stderr)
        print("提示：Host EXE 或其它客户端占用 M UART0 时无法测量。", file=sys.stderr)
        return 2

    result = {
        "port": args.port,
        "baud": args.baud,
        "count": args.count,
        "gapMs": args.gap_ms,
        "startedAt": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    try:
        print(f"只读测量 {args.port} @ {args.baud} 8N1；未发送复位命令，DTR/RTS 已置低。")
        device, _, _ = link.query(OP_DEVICE, timeout=args.timeout)
        version, _, _ = link.query(OP_VERSION, timeout=args.timeout)
        result["device"] = device.hex()
        result["apiLevel"] = int.from_bytes(version, "little")
        print(f"DEVICE={device.hex()}（0x01=mouse）  API level={result['apiLevel']}")

        # 预热：让 M 的 UART0 任务进入稳定状态，避免首个样本混入冷路径。
        for _ in range(10):
            link.query(OP_PHYS_BUTTONS, timeout=args.timeout)
        link.drain()

        whole, states = measure_query_latency(link, args.count, args.gap_ms, args.timeout)
        result["queryRoundTripUs"] = summarize(whole)
        result["observedStates"] = {str(key): value for key, value in states.items()}
        print_summary("0x55 phys_buttons 查询往返（写入→读完整帧）", result["queryRoundTripUs"])

        if args.stream_clicks > 0:
            press, release, payloads = measure_stream_events(link, args.stream_clicks, args.timeout)
            result["streamPressUs"] = summarize(press)
            result["streamReleaseUs"] = summarize(release)
            result["streamPayloads"] = payloads
            print_summary("0x10 订阅后 click 注入→「按下」事件字节到达", result["streamPressUs"])
            print_summary("同上，同一次 click 的「松开」事件字节到达", result["streamReleaseUs"])
            print(f"事件 payload 样本：{payloads[:4]}（kind,button,value）")
    except Exception as exc:  # noqa: BLE001 - 测量失败也要留下已得数据
        result["error"] = f"{type(exc).__name__}: {exc}"
        print(f"测量中断：{result['error']}", file=sys.stderr)
    finally:
        link.close()

    result["finishedAt"] = time.strftime("%Y-%m-%d %H:%M:%S")
    output = args.output
    if output is None:
        output = Path("artifacts/tests/makcu-button-query") / (
            f"{args.port.replace(':', '')}-{time.strftime('%Y%m%d-%H%M%S')}.json"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"JSON 已写入 {output}")
    return 1 if "error" in result else 0


if __name__ == "__main__":
    sys.exit(main())
