"""测量远程 UDP 鼠标输入的延迟、5 槽平滑和连续命令重叠效果。

脚本在运行机本机每 1 ms 时间桶采样一次鼠标坐标，向运行 HidBridge.Host.exe 的
地址发送相对位移，并生成 CSV、JSON 和两个 SVG 图。测试结束后用反向 UDP 命令
恢复起点。

使用前提（不满足时退出码 2）::

    * 电脑侧板(P)的 USB 输出接到**运行本脚本的这台机器**，克隆鼠标已枚举；
    * HidBridge.Host.exe 正在运行，且 UDP 输入端口可访问（默认本机 24814）。

判定标准（不符合时退出码 1）::

    * 每类用例的首次移动延迟 < 10 ms（相对该用例第一条命令的发出时刻）；
    * 实际位移与预期一致：单次用例 20px、连续用例 60px。

默认对**每一类用例连续测量 10 轮**（`--repeat` 可改），且**每一轮都必须达标**：
任何一轮不达标即整体 FAIL，结论里会列出全部未达标轮次。测量结束后额外输出各指标的
**平均结果**（首动延迟的均值/最小/最大、逐命令延迟均值、50% 位移耗时均值、实际位移均值）。
50% 位移耗时、逐命令延迟、每步位移等仍只作为过程信息写入 JSON/CSV/SVG 与平均结果，
不参与逐轮判定。
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
import ctypes
import csv
import json
import math
import socket
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable

# 判定阈值与前置检查标识。
FIRST_MOVE_LIMIT_MS = 10.0
# 开始测量前先等这么久，然后把准心移到屏幕中心再测：圆形用例半径 100 px，
# 从屏幕边缘起步会被边界裁掉位移，终点核对随之失去意义。
CENTER_SETTLE_S = 3.0
# 克隆设备型号随被代理的鼠标变化：046D:C092（有线 G102）与 046D:C539（Lightspeed
# 接收器）都在实机出现过。因此前置检查默认按“当前枚举到的任意 USB 罗技节点”判定，
# 需要精确型号时用 --expect-vidpid 046D:xxxx。
CLONE_VENDOR_HINT = "VID_046D"
CLONE_USB_PREFIX = "USB\\VID_046D&PID_"
CLONE_INSTANCE_HINT = "VID_046D&PID_C092"  # 历史样本，仅用于文案提示
SINGLE_DISTANCE_PX = 20
CONTINUOUS_COMMANDS = 3
# 长时移动用例（2026-09-27 新增，同日改为 1 像素步进）：每 1 ms 发一次位移、持续 10 s，
# 沿半径 100 px 的圆**逐像素**行走——每步只走 1 个像素单位（dx/dy ∈ {-1,0,1}）。
# 10 s × 1000 步 ≈ 10000 步 ≈ 10000 px 路程 ≈ 15.9 圈（周长 2πr ≈ 628 px）。
# 这样既能压出长时高频负载，又能用「实际终点 vs 预期终点」验证每个 1 px 微步是否被
# 逐条精确执行（而不是像变长增量那样只看总量）。
# 采样降到 2 ms：1 ms 桶已经被发送占满，再叠加一次 GetCursorPos 会让调度失真。
CIRCLE_RADIUS_PX = 100
CIRCLE_DURATION_S = 10.0
CIRCLE_STEP_US = 1_000
CIRCLE_SAMPLE_US = 2_000
CIRCLE_STEP_PX = 1                   # 每步移动的像素单位（曼哈顿步长）
CIRCLE_END_TOLERANCE_PX = 2          # 终点允许偏差（指针加速/取整可能带来 1~2 px）
# 长时移动的检查阈值（超过即计入 failures）
CIRCLE_MAX_GAP_US = 100_000          # 位移停顿 > 100 ms 视为卡顿
CIRCLE_MAX_SEND_LAG_US = 5_000       # 单次发送相对计划时刻滞后 > 5 ms 视为节奏失守


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


_USER32 = ctypes.WinDLL("user32", use_last_error=True)
_USER32.GetCursorPos.argtypes = [ctypes.POINTER(Point)]
_USER32.GetCursorPos.restype = ctypes.c_bool
_USER32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
_USER32.SetCursorPos.restype = ctypes.c_bool
_USER32.GetSystemMetrics.argtypes = [ctypes.c_int]
_USER32.GetSystemMetrics.restype = ctypes.c_int
SM_CXSCREEN = 0
SM_CYSCREEN = 1


@dataclass(frozen=True)
class Sample:
    elapsed_us: int
    x: int
    y: int


@dataclass(frozen=True)
class Command:
    index: int
    planned_us: int
    sent_us: int
    completed_us: int
    dx: int
    dy: int


def cursor_position() -> tuple[int, int]:
    point = Point()
    if not _USER32.GetCursorPos(ctypes.byref(point)):
        raise ctypes.WinError(ctypes.get_last_error())
    return int(point.x), int(point.y)


def restore_cursor(x: int, y: int) -> None:
    for _ in range(20):
        _USER32.SetCursorPos(x, y)
        if cursor_position() == (x, y):
            return
        time.sleep(0.001)
    current = cursor_position()
    raise RuntimeError(f"无法恢复鼠标位置：目标=({x},{y})，当前=({current[0]},{current[1]})")


def screen_center() -> tuple[int, int]:
    """主屏中心坐标（多屏环境取主屏即可满足半径 100 px 的圆用例）。"""
    return (_USER32.GetSystemMetrics(SM_CXSCREEN) // 2,
            _USER32.GetSystemMetrics(SM_CYSCREEN) // 2)


def move_cursor_to_center() -> tuple[int, int]:
    """把准心移到屏幕中心并确认到位，返回中心坐标。"""
    center_x, center_y = screen_center()
    restore_cursor(center_x, center_y)
    return center_x, center_y


def present_logitech_nodes() -> list[str] | None:
    """列出本机当前枚举的罗技（VID_046D）设备 InstanceId；无法查询时返回 None。

    用 `-PresentOnly`：只统计当前真实存在的节点，已拔掉的残留记录（Status=Unknown）
    不会算进来——这正是原来“明明接过 C092 却说没有节点”的原因之一。
    """
    query = (
        "Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
        f"Where-Object {{ $_.InstanceId -like '*{CLONE_VENDOR_HINT}*' }} | "
        "Select-Object -ExpandProperty InstanceId"
    )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", query],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()]


def normalize_expected_vidpid(value: str) -> str:
    """把 `046D:C539` / `VID_046D&PID_C539` 统一成 InstanceId 子串写法。"""
    text = value.strip().upper()
    if not text:
        return ""
    if text.startswith("VID_"):
        return text
    if ":" in text:
        vid, pid = text.split(":", 1)
        return f"VID_{vid}&PID_{pid}"
    return text


def clone_device_present(expected: str, nodes: list[str]) -> bool:
    """判断本机当前是否枚举了电脑侧板(P)克隆出来的设备。

    expected 为空时按“存在任意 USB\\VID_046D&PID_* 节点”判定：只认真实 USB 设备
    节点，G HUB 的 LGHUBDEVICE\\VID_046D&PID_C231/C232 虚拟设备不会误判为克隆。
    给了 expected 时要求 InstanceId 含该子串（精确到型号）。
    """
    if expected:
        return any(expected in node.upper() for node in nodes)
    return any(node.upper().startswith(CLONE_USB_PREFIX) for node in nodes)


def first_move_latency_ms(result: dict) -> float | None:
    """取该用例首条命令的实际首动延迟（ms）；未观测到位移时返回 None。

    只取首条命令：连续用例中后续命令发出时前一条命令的平滑槽位仍在到达，
    用它们算首动会把上一命令的残留位移误记为本命令的延迟。
    """
    per_command = result.get("per_command") or []
    if not per_command:
        return None
    latency_us = per_command[0].get("start_latency_us")
    return None if latency_us is None else latency_us / 1000.0


def judge_case(label: str, result: dict, expected_dx: int) -> list[str]:
    """按验收标准判定一次用例，返回失败原因列表（空列表表示通过）。"""
    failures: list[str] = []
    latency_ms = first_move_latency_ms(result)
    if latency_ms is None:
        failures.append(
            f"{label}：未观测到首次位移（要求 < {FIRST_MOVE_LIMIT_MS:.1f}ms）；"
            f"请确认电脑侧板(P)输出到本机、克隆已枚举且 HidBridge.Host.exe 正在运行")
    elif latency_ms >= FIRST_MOVE_LIMIT_MS:
        failures.append(
            f"{label}：首次移动延迟 {latency_ms:.2f}ms 未低于 {FIRST_MOVE_LIMIT_MS:.1f}ms")
    else:
        print(f"[判定] {label} 首次移动延迟 {latency_ms:.2f}ms < "
              f"{FIRST_MOVE_LIMIT_MS:.1f}ms：合格", flush=True)

    observed = result.get("total_observed_dx")
    if observed != expected_dx:
        failures.append(f"{label}：实际位移 {observed}px 与预期 {expected_dx}px 不符")
        if observed is not None and abs(observed - expected_dx) == 1 and \
                len(result.get("per_command") or []) > 1:
            print("[提示] 多命令用例偏差 1px 时先检查 Windows 指针加速"
                  "（设置→鼠标→提高指针精确度）与上一用例残留的平滑槽位；"
                  "单次用例的位移判定不受影响。", flush=True)
    else:
        print(f"[判定] {label} 实际位移 {observed}px 与预期一致：合格", flush=True)
    return failures


def _mean(values: list[float | int | None]) -> float | None:
    usable = [float(value) for value in values if value is not None]
    return None if not usable else sum(usable) / len(usable)


def _fmt_ms(value: float | None) -> str:
    return "-" if value is None else f"{value:.2f}"


def average_runs(results: list[dict]) -> dict:
    """对同一用例的多轮测量求平均，供结论里的“平均结果”使用。

    只统计可用的数值指标：首动延迟（每轮取首条命令）、逐命令延迟、50% 位移耗时、
    整段位移耗时、实际总位移。任一轮缺失的指标不参与该项平均（不是按 0 计入）。
    """
    if not results:
        return {"runs": 0}
    first_moves = [first_move_latency_ms(result) for result in results]
    command_latencies = [
        command["start_latency_us"] / 1000.0
        for result in results
        for command in (result.get("per_command") or [])
        if command.get("start_latency_us") is not None
    ]
    half_times = [
        command["half_time_us"] / 1000.0
        for result in results
        for command in (result.get("per_command") or [])
        if command.get("half_time_us") is not None
    ]
    durations = [
        result["motion_duration_us"] / 1000.0
        for result in results
        if result.get("motion_duration_us") is not None
    ]
    observed = [
        result["total_observed_dx"]
        for result in results
        if result.get("total_observed_dx") is not None
    ]
    usable_first = [value for value in first_moves if value is not None]
    return {
        "runs": len(results),
        "runs_with_first_move": len(usable_first),
        "first_move_ms_avg": _mean(usable_first),
        "first_move_ms_min": min(usable_first) if usable_first else None,
        "first_move_ms_max": max(usable_first) if usable_first else None,
        "per_command_latency_ms_avg": _mean(command_latencies),
        "half_travel_ms_avg": _mean(half_times),
        "motion_duration_ms_avg": _mean(durations),
        "observed_dx_avg": _mean(observed),
    }


def print_average(title: str, average: dict) -> None:
    observed = average.get("observed_dx_avg")
    observed_text = "-" if observed is None else f"{observed:.2f}"
    print(f"[平均] {title}：共 {average.get('runs', 0)} 轮，"
          f"首动均值 {_fmt_ms(average.get('first_move_ms_avg'))} ms"
          f"（最小 {_fmt_ms(average.get('first_move_ms_min'))} / "
          f"最大 {_fmt_ms(average.get('first_move_ms_max'))} ms），"
          f"逐命令延迟均值 {_fmt_ms(average.get('per_command_latency_ms_avg'))} ms，"
          f"50% 位移均值 {_fmt_ms(average.get('half_travel_ms_avg'))} ms，"
          f"位移耗时均值 {_fmt_ms(average.get('motion_duration_ms_avg'))} ms，"
          f"实际位移均值 {observed_text} px", flush=True)


class UdpSender:
    def __init__(self, host: str, port: int) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._address = (host, port)

    def send(
        self,
        dx: int,
        dy: int,
        clock_ns: Callable[[], int] = time.perf_counter_ns,
    ) -> tuple[int, int]:
        payload = json.dumps(
            {"dx": int(dx), "dy": int(dy), "wheel": 0, "pan": 0},
            separators=(",", ":"),
        ).encode("utf-8")
        send_started_ns = clock_ns()
        self._socket.sendto(payload, self._address)
        return send_started_ns, clock_ns()

    def close(self) -> None:
        self._socket.close()


def send_command(
    sender: UdpSender,
    start_ns: int,
    commands: list[Command],
    dx: int,
    dy: int,
    planned_us: int,
    clock_ns: Callable[[], int] = time.perf_counter_ns,
) -> Command:
    send_started_ns, completed_ns = sender.send(dx, dy, clock_ns)
    sent_us = (send_started_ns - start_ns) // 1_000
    completed_us = (completed_ns - start_ns) // 1_000
    command = Command(
        len(commands) + 1,
        planned_us,
        sent_us,
        completed_us,
        dx,
        dy,
    )
    commands.append(command)
    return command


def capture_cursor_sample(
    start_ns: int,
    read_cursor: Callable[[], tuple[int, int]] = cursor_position,
    clock_ns: Callable[[], int] = time.perf_counter_ns,
) -> Sample:
    x, y = read_cursor()
    elapsed_us = (clock_ns() - start_ns) // 1_000
    return Sample(elapsed_us, x, y)


def changed_samples(samples: list[Sample]) -> list[tuple[Sample, int, int]]:
    result: list[tuple[Sample, int, int]] = []
    for previous, current in zip(samples, samples[1:]):
        dx = current.x - previous.x
        dy = current.y - previous.y
        if dx or dy:
            result.append((current, dx, dy))
    return result


def command_metrics(
    samples: list[Sample], commands: list[Command], start_x: int, targets: list[int], final_target: int
) -> dict:
    analysis_start_us = max(0, commands[0].sent_us - 5_000)
    target_sample = next((sample for sample in samples if sample.x >= final_target), None)
    analysis_end_us = None if target_sample is None else target_sample.elapsed_us + 5_000
    bounded_samples = [
        sample for sample in samples
        if sample.elapsed_us >= analysis_start_us and (analysis_end_us is None or sample.elapsed_us <= analysis_end_us)
    ]
    changes = changed_samples(bounded_samples)
    metrics = []
    for command, target in zip(commands, targets):
        after_send = [sample for sample in samples if sample.elapsed_us >= command.sent_us]
        first_change = next((sample for sample, _, _ in changes if sample.elapsed_us >= command.sent_us), None)
        threshold = start_x + ((target - start_x) // 2)
        half = next((sample for sample in after_send if sample.x >= threshold), None)
        metrics.append(
            {
                "index": command.index,
                "planned_us": command.planned_us,
                "sent_us": command.sent_us,
                "send_completed_us": command.completed_us,
                "send_call_duration_us": command.completed_us - command.sent_us,
                "target_x": target,
                "start_latency_us": None if first_change is None else first_change.elapsed_us - command.sent_us,
                "half_target_x": threshold,
                "half_time_us": None if half is None else max(0, half.elapsed_us - command.sent_us),
                "already_at_half_when_sent": bool(half and half.elapsed_us <= command.sent_us),
            }
        )
    motion_changes = [item for item in changes if item[0].x != start_x]
    first_motion = motion_changes[0][0] if motion_changes else None
    last_motion = motion_changes[-1][0] if motion_changes else None
    return {
        "commands": [asdict(command) for command in commands],
        "per_command": metrics,
        "step_distances": [dx for _, dx, _ in changes if dx],
        "first_motion_us": None if first_motion is None else first_motion.elapsed_us,
        "last_motion_us": None if last_motion is None else last_motion.elapsed_us,
        "motion_duration_us": None
        if first_motion is None or last_motion is None
        else last_motion.elapsed_us - first_motion.elapsed_us,
        "final_target_x": final_target,
        "observed_final_x": bounded_samples[-1].x if bounded_samples else None,
        "total_observed_dx": (bounded_samples[-1].x - start_x) if bounded_samples else None,
        "analysis_end_us": analysis_end_us,
        "analysis_start_us": analysis_start_us,
    }


def svg_plot(path: Path, title: str, samples: list[Sample], commands: list[Command], start_x: int, start_us: int, end_us: int | None) -> None:
    width, height = 1_100, 560
    left, right, top, bottom = 80, 30, 55, 70
    plot_w, plot_h = width - left - right, height - top - bottom
    if not samples:
        return
    if end_us is not None:
        samples = [sample for sample in samples if sample.elapsed_us <= end_us]
    samples = [Sample(sample.elapsed_us - start_us, sample.x, sample.y) for sample in samples]
    commands = [
        Command(
            command.index,
            command.planned_us - start_us,
            command.sent_us - start_us,
            command.completed_us - start_us,
            command.dx,
            command.dy,
        )
        for command in commands
    ]
    max_t = max(samples[-1].elapsed_us, 1)
    values = [sample.x - start_x for sample in samples]
    min_v, max_v = min(values + [0]), max(values + [0])
    span = max(1, max_v - min_v)

    def px(t: int) -> float:
        return left + (t / max_t) * plot_w

    def py(value: int) -> float:
        return top + (max_v - value) / span * plot_h

    points = " ".join(f"{px(sample.elapsed_us):.1f},{py(sample.x - start_x):.1f}" for sample in samples)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="28" font-family="Segoe UI" font-size="20" fill="#111827">{title}</text>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#374151"/>',
        f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#374151"/>',
        f'<text x="{left + plot_w - 110}" y="{height - 25}" font-family="Segoe UI" font-size="13">时间 (ms)</text>',
        f'<text x="12" y="{top + 12}" font-family="Segoe UI" font-size="13">位移 (px)</text>',
    ]
    for command in commands:
        x = px(command.sent_us)
        lines.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top + plot_h}" stroke="#dc2626" stroke-dasharray="5,4"/>')
        lines.append(f'<text x="{x + 4:.1f}" y="{top + 16}" font-family="Segoe UI" font-size="12" fill="#b91c1c">发送{command.index}</text>')
    for tick_us in range(0, max_t + 1_000, 1_000):
        x = px(tick_us)
        if x > left + plot_w:
            break
        lines.append(f'<line x1="{x:.1f}" y1="{top + plot_h}" x2="{x:.1f}" y2="{top + plot_h + 6}" stroke="#374151"/>')
        lines.append(f'<text x="{x:.1f}" y="{top + plot_h + 22}" text-anchor="middle" font-family="Segoe UI" font-size="11">{tick_us // 1000}</text>')
    lines.append(f'<polyline points="{points}" fill="none" stroke="#2563eb" stroke-width="2"/>')
    for value in sorted(set(values)):
        y = py(value)
        lines.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        lines.append(f'<text x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" font-family="Segoe UI" font-size="12">{value}</text>')
    lines.append("</svg>")
    path.write_text("\n".join(lines), encoding="utf-8")


def write_csv(path: Path, samples: list[Sample]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("elapsed_us", "x", "y", "dx", "dy"))
        previous = samples[0] if samples else None
        for sample in samples:
            writer.writerow((sample.elapsed_us, sample.x, sample.y, 0 if previous is None else sample.x - previous.x, 0 if previous is None else sample.y - previous.y))
            previous = sample


def write_command_csv(path: Path, commands: list[Command]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow((
            "index",
            "planned_us",
            "send_started_us",
            "send_completed_us",
            "send_call_duration_us",
            "send_lag_us",
            "dx",
            "dy",
        ))
        for command in commands:
            writer.writerow((
                command.index,
                command.planned_us,
                command.sent_us,
                command.completed_us,
                command.completed_us - command.sent_us,
                max(0, command.sent_us - command.planned_us),
                command.dx,
                command.dy,
            ))


def run_case(
    name: str,
    sender: UdpSender,
    settle_us: int,
    interval_us: int,
    count: int,
    *,
    read_cursor: Callable[[], tuple[int, int]] = cursor_position,
    clock_ns: Callable[[], int] = time.perf_counter_ns,
) -> tuple[dict, list[Sample], list[Command], int]:
    start_x, start_y = read_cursor()
    commands: list[Command] = []
    start_ns = clock_ns()
    send_deadlines_us = [settle_us + index * interval_us for index in range(count)]
    end_us = settle_us + (count - 1) * interval_us + 80_000
    next_sample_us = 0
    samples: list[Sample] = []
    next_command = 0
    while next_sample_us <= end_us:
        now_us = (clock_ns() - start_ns) // 1_000
        while next_command < count and now_us >= send_deadlines_us[next_command]:
            send_command(
                sender,
                start_ns,
                commands,
                20,
                0,
                send_deadlines_us[next_command],
                clock_ns,
            )
            next_command += 1
        if now_us >= next_sample_us:
            samples.append(capture_cursor_sample(start_ns, read_cursor, clock_ns))
            next_sample_us += 1_000
        # 采样和发送调度路径禁止 sleep：Windows 的线程睡眠粒度可能约为 15.6 ms，
        # 会直接跳过多个 1 ms 桶。这里使用 perf_counter_ns() 忙等到下一个绝对时间点。
    if commands:
        sender.send(-20 * count, 0)
        time.sleep(0.08)
    restore_cursor(start_x, start_y)
    end_x, end_y = read_cursor()
    targets = [start_x + 20 * index for index in range(1, count + 1)]
    result = command_metrics(samples, commands, start_x, targets, start_x + 20 * count)
    result.update({"name": name, "start": [start_x, start_y], "end": [end_x, end_y], "end_restored": [end_x, end_y] == [start_x, start_y]})
    return result, samples, commands, start_x


def circle_metrics(
    samples: list[Sample],
    commands: list[Command],
    walk_dx: int,
    walk_dy: int,
    expected_end: tuple[int, int],
    actual_end: tuple[int, int],
    end_delta: tuple[int, int],
) -> dict:
    """长时移动指标。时间只表示本机发包调用和 Windows 光标采样的观测。"""
    changes = changed_samples(samples)
    sample_times = [sample.elapsed_us for sample in samples]
    sample_intervals_us = [right - left for left, right in zip(sample_times, sample_times[1:])]
    previous_us = commands[0].sent_us if commands else 0
    max_gap_us = 0
    max_gap_sample_interval_us = 0
    max_gap_sample_count = 0
    for sample, _, _ in changes:
        gap_us = sample.elapsed_us - previous_us
        if gap_us > max_gap_us:
            first_sample = bisect_right(sample_times, previous_us)
            end_sample = bisect_right(sample_times, sample.elapsed_us)
            local_sample_times = sample_times[first_sample:end_sample]
            local_max_sample_interval_us = 0
            previous_sample_us = previous_us
            for sample_us in local_sample_times:
                local_max_sample_interval_us = max(
                    local_max_sample_interval_us,
                    sample_us - previous_sample_us,
                )
                previous_sample_us = sample_us
            local_max_sample_interval_us = max(
                local_max_sample_interval_us,
                sample.elapsed_us - previous_sample_us,
            )
            max_gap_us = gap_us
            max_gap_sample_interval_us = local_max_sample_interval_us
            max_gap_sample_count = len(local_sample_times)
        previous_us = sample.elapsed_us

    send_lags_us = [max(0, command.sent_us - command.planned_us) for command in commands]
    send_call_durations_us = [
        max(0, command.completed_us - command.sent_us) for command in commands
    ]
    send_intervals_us = [
        current.sent_us - previous.sent_us
        for previous, current in zip(commands, commands[1:])
    ]

    def percentile(values: list[int], pct: float) -> int | None:
        ordered = sorted(values)
        if not ordered:
            return None
        index = min(len(ordered) - 1, int(round((pct / 100.0) * (len(ordered) - 1))))
        return ordered[index]

    return {
        "commands_sent": len(commands),
        "sample_count": len(samples),
        "duration_us": samples[-1].elapsed_us if samples else None,
        "send_lag_us_avg": (sum(send_lags_us) / len(send_lags_us)) if send_lags_us else None,
        "send_lag_us_p99": percentile(send_lags_us, 99.0),
        "send_lag_us_max": max(send_lags_us) if send_lags_us else None,
        "send_behind_count": sum(1 for lag in send_lags_us if lag > CIRCLE_MAX_SEND_LAG_US),
        "send_call_duration_us_avg": (
            sum(send_call_durations_us) / len(send_call_durations_us)
            if send_call_durations_us else None
        ),
        "send_call_duration_us_p99": percentile(send_call_durations_us, 99.0),
        "send_call_duration_us_max": max(send_call_durations_us) if send_call_durations_us else None,
        "send_interval_us_avg": (
            sum(send_intervals_us) / len(send_intervals_us) if send_intervals_us else None
        ),
        "send_interval_us_p99": percentile(send_intervals_us, 99.0),
        "send_interval_us_max": max(send_intervals_us) if send_intervals_us else None,
        "max_sample_interval_us": max(sample_intervals_us) if sample_intervals_us else None,
        "p99_sample_interval_us": percentile(sample_intervals_us, 99.0),
        "sample_intervals_over_2x_period": sum(
            1 for interval in sample_intervals_us if interval > CIRCLE_SAMPLE_US * 2
        ),
        "motion_change_count": len(changes),
        "max_no_motion_gap_us": max_gap_us,
        "max_no_motion_gap_sample_interval_us": max_gap_sample_interval_us,
        "max_no_motion_gap_sample_count": max_gap_sample_count,
        "total_path_px": sum(abs(dx) + abs(dy) for _, dx, dy in changes),
        "walk_steps": len(commands),
        "walk_px": len(commands) * CIRCLE_STEP_PX,
        "net_walk_dx": walk_dx,
        "net_walk_dy": walk_dy,
        "expected_end": [expected_end[0], expected_end[1]],
        "actual_end": [actual_end[0], actual_end[1]],
        "end_delta": [end_delta[0], end_delta[1]],
        "end_delta_px": max(abs(end_delta[0]), abs(end_delta[1])),
    }


def print_circle_report(result: dict) -> None:
    def ms(value: float | None) -> str:
        return "-" if value is None else f"{value / 1000.0:.2f}"

    print(f"[长时移动] 发出 {result.get('commands_sent')} 条命令（每 1ms 一条）／"
          f"采样 {result.get('sample_count')} 点／"
          f"观测到位移变化 {result.get('motion_change_count')} 次", flush=True)
    print(f"  发送节奏：平均滞后 {ms(result.get('send_lag_us_avg'))} ms，"
          f"p99 {ms(result.get('send_lag_us_p99'))} ms，"
          f"最大 {ms(result.get('send_lag_us_max'))} ms，"
          f"超 {CIRCLE_MAX_SEND_LAG_US / 1000.0:.0f}ms 的有 "
          f"{result.get('send_behind_count')} 次", flush=True)
    print(f"  发送调用：最大耗时 {ms(result.get('send_call_duration_us_max'))} ms，"
          f"相邻发送最大间隔 {ms(result.get('send_interval_us_max'))} ms；"
          f"采样最大间隔 {ms(result.get('max_sample_interval_us'))} ms，"
          f"超过 {CIRCLE_SAMPLE_US * 2 / 1000.0:.0f}ms 的有 "
          f"{result.get('sample_intervals_over_2x_period')} 次", flush=True)
    print(f"  连续性：最长无位移 {ms(result.get('max_no_motion_gap_us'))} ms"
          f"（阈值 {CIRCLE_MAX_GAP_US / 1000.0:.0f} ms；该窗口内最大采样间隔 "
          f"{ms(result.get('max_no_motion_gap_sample_interval_us'))} ms，"
          f"采样点 {result.get('max_no_motion_gap_sample_count')}）；"
          f"观测路程 {result.get('total_path_px')} px；"
          f"发出步数 {result.get('walk_steps')}（每步 {CIRCLE_STEP_PX} px）", flush=True)
    end_delta = result.get("end_delta") or [None, None]
    print(f"  终点核对：预期 {result.get('expected_end')} → 实际 "
          f"{result.get('actual_end')}，偏差 dx={end_delta[0]} dy={end_delta[1]}"
          f"（最大 {result.get('end_delta_px')} px，容差 {CIRCLE_END_TOLERANCE_PX} px）",
          flush=True)


def judge_circle(result: dict) -> list[str]:
    """判定长时移动用例；返回失败原因列表（空列表表示通过）。"""
    label = f"长时移动（{CIRCLE_DURATION_S:.0f}s 每 1ms 一次画 r={CIRCLE_RADIUS_PX}px 圆）"
    failures: list[str] = []
    expected_commands = int(round(CIRCLE_DURATION_S * 1_000_000 / CIRCLE_STEP_US))
    if result.get("commands_sent") != expected_commands:
        failures.append(
            f"{label}：实际发出 {result.get('commands_sent')} 条命令，预期 {expected_commands} 条")
    gap = result.get("max_no_motion_gap_us")
    if gap is None:
        failures.append(
            f"{label}：完全没有观测到位移；请确认电脑侧板(P)输出到本机、"
            f"克隆已枚举且 HidBridge.Host.exe 正在运行")
    elif gap > CIRCLE_MAX_GAP_US:
        sample_gap = result.get("max_no_motion_gap_sample_interval_us") or 0
        sample_count = result.get("max_no_motion_gap_sample_count")
        failures.append(f"{label}：出现 {gap / 1000.0:.1f}ms 的位移停顿，超过阈值 "
                        f"{CIRCLE_MAX_GAP_US / 1000.0:.0f}ms；同一窗口内最大采样间隔 "
                        f"{sample_gap / 1000.0:.1f}ms，采样点 {sample_count}。"
                        "这是本机光标观测，不能单独定位输入链路。")
    lag = result.get("send_lag_us_max")
    if lag is not None and lag > CIRCLE_MAX_SEND_LAG_US:
        failures.append(f"{label}：发送节奏最大滞后 {lag / 1000.0:.1f}ms，超过阈值 "
                        f"{CIRCLE_MAX_SEND_LAG_US / 1000.0:.0f}ms")
    end_delta_px = result.get("end_delta_px")
    if end_delta_px is None:
        failures.append(f"{label}：未取得终点数据，无法核对最终位置")
    elif end_delta_px > CIRCLE_END_TOLERANCE_PX:
        failures.append(
            f"{label}：最终位置与预期相差 {end_delta_px}px（容差 {CIRCLE_END_TOLERANCE_PX}px）"
            f"——预期 {result.get('expected_end')}、实际 {result.get('actual_end')}，"
            "位移校验失败；需排查外部鼠标操作、屏幕边界、指针加速及输入链路")
    if not failures:
        print(f"[判定] {label}：发送节奏、位移连续性与终点位置均达标", flush=True)
    return failures


def run_circle_case(
    sender: UdpSender,
    *,
    read_cursor: Callable[[], tuple[int, int]] = cursor_position,
    clock_ns: Callable[[], int] = time.perf_counter_ns,
) -> tuple[dict, list[Sample], list[Command], int]:
    """长时移动用例：每 1 ms 发一次、持续 10 s，沿 r=100px 的圆**逐像素**行走。

    每步只走 1 个像素单位：先按"每步弧长 1 px"推进角度得到理想位置，再从 8 邻域
    （dx/dy ∈ {-1,0,1}）里挑出离理想位置最近的一步发出去。这样每个 UDP 包都是微步，
    既能验证"1 px 的移动不会被吞掉"，也能用累积出来的预期终点去核对最终落点。

    计时与短用例同源：perf_counter_ns() 忙等到**绝对**时间点（step × 1ms），绝不 sleep。
    """
    start_x, start_y = read_cursor()
    total_steps = int(round(CIRCLE_DURATION_S * 1_000_000 / CIRCLE_STEP_US))
    arc_step = CIRCLE_STEP_PX / float(CIRCLE_RADIUS_PX)   # 每步对应的圆心角
    neighbor_steps = [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy]
    commands: list[Command] = []
    samples: list[Sample] = []
    start_ns = clock_ns()
    walk_x = 0      # 相对起点的累积位移，也就是预期终点的偏移量
    walk_y = 0
    step = 0
    next_plan_us = CIRCLE_STEP_US
    next_sample_us = 0
    end_us = total_steps * CIRCLE_STEP_US
    while step < total_steps or next_sample_us <= end_us:
        now_us = (clock_ns() - start_ns) // 1_000
        while step < total_steps and now_us >= next_plan_us:
            angle = arc_step * (step + 1)
            ideal_x = CIRCLE_RADIUS_PX * math.cos(angle)
            ideal_y = CIRCLE_RADIUS_PX * math.sin(angle)
            # 8 邻域里挑离理想位置最近的一步 → 保证每步恰好 1 个像素单位。
            best_dx, best_dy = neighbor_steps[0]
            best_distance: float | None = None
            for dx, dy in neighbor_steps:
                distance = (walk_x + dx - ideal_x) ** 2 + (walk_y + dy - ideal_y) ** 2
                if best_distance is None or distance < best_distance:
                    best_distance = distance
                    best_dx, best_dy = dx, dy
            walk_x += best_dx
            walk_y += best_dy
            send_command(
                sender,
                start_ns,
                commands,
                best_dx,
                best_dy,
                next_plan_us,
                clock_ns,
            )
            step += 1
            next_plan_us += CIRCLE_STEP_US
        if now_us >= next_sample_us:
            samples.append(capture_cursor_sample(start_ns, read_cursor, clock_ns))
            next_sample_us += CIRCLE_SAMPLE_US

    # 终点核对：链路与平滑槽排空都需要时间，等位置**连续两次读数一致**再判定，
    # 否则会把"还没走完的平滑残留"误算成终点偏差（最多等 1.5 s）。
    expected_end = (start_x + walk_x, start_y + walk_y)
    actual_end = read_cursor()
    for _ in range(30):
        time.sleep(0.05)
        current = read_cursor()
        if current == actual_end:
            break
        actual_end = current
    end_delta = (actual_end[0] - expected_end[0], actual_end[1] - expected_end[1])
    # 再走回起点（恢复用户原来的光标位置）。
    if walk_x or walk_y:
        sender.send(-walk_x, -walk_y)
        time.sleep(0.15)
    restore_cursor(start_x, start_y)

    result = circle_metrics(samples, commands, walk_x, walk_y,
                            expected_end, actual_end, end_delta)
    result.update({
        "name": "circle_r100_10s_1px_step",
        "start": [start_x, start_y],
        "end": [actual_end[0], actual_end[1]],
        "end_restored": read_cursor() == (start_x, start_y),
        "analysis_start_us": commands[0].sent_us if commands else 0,
        "analysis_end_us": samples[-1].elapsed_us if samples else None,
    })
    return result, samples, commands, start_x


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1",
                        help="运行 HidBridge.Host.exe 的地址（默认本机 127.0.0.1；"
                             "EXE 在另一台机器时改成那台的地址）")
    parser.add_argument("--port", type=int, default=24814)
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/tests/udp-smoothing-measurement"))
    parser.add_argument("--expect-vidpid", default="",
                        help="前置检查期望的克隆型号（如 046D:C539 或 VID_046D&PID_C539）；"
                             "缺省自动：本机当前枚举到任意 USB 罗技设备即通过")
    parser.add_argument("--force", action="store_true",
                        help="跳过“本机是否已枚举电脑侧板克隆设备”的前置检查")
    parser.add_argument("--repeat", type=int, default=10,
                        help="每类用例连续测量的轮数（默认 10；每一轮都必须达标，"
                             "任何一轮不达标即整体 FAIL）")
    parser.add_argument("--circle-repeat", type=int, default=1,
                        help="长时移动用例（10 秒画 r=100px 圆）的轮数，默认 1；"
                             "每轮会发 1 万条命令，调大前先确认发送端扛得住")
    parser.add_argument("--print-json", action="store_true",
                        help="额外在标准输出打印完整判定 JSON（缺省只写入 summary.json）")
    args = parser.parse_args()

    if args.force:
        # 明确警告：跳过前置检查时 UDP 可能根本没有接收方，此时测到的位移会来自
        # 使用者手动移动鼠标——这种数字不能当结论用（2026-09-27 踩过一次）。
        print("[前置] 已用 --force 跳过克隆设备检查：若 HidBridge.Host.exe 未运行，"
              "本轮位移可能全部来自手动移动鼠标，结果不可作为结论。", flush=True)
    if not args.force:
        expected = normalize_expected_vidpid(args.expect_vidpid)
        nodes = present_logitech_nodes()
        if nodes is None:
            print("[前置] 无法查询 PnP 设备，跳过前置检查（可用 --force 显式跳过）", flush=True)
        elif clone_device_present(expected, nodes):
            print(f"[前置] 已在本机枚举到克隆设备（匹配 {expected or CLONE_USB_PREFIX + '*'}）",
                  flush=True)
        else:
            hint = expected or (CLONE_USB_PREFIX + "*")
            print(f"前置条件不满足：本机当前没有匹配 {hint} 的设备节点。本脚本只在电脑侧板(P)"
                  "输出到本机、克隆设备已枚举、且 HidBridge.Host.exe 正在运行时可用。",
                  file=sys.stderr, flush=True)
            if nodes:
                print("  当前枚举到的 046D 节点：", file=sys.stderr, flush=True)
                for node in nodes:
                    print("    " + node, file=sys.stderr, flush=True)
            else:
                print("  当前枚举到的 046D 节点：无（电脑侧板未接本机或克隆未挂载）",
                      file=sys.stderr, flush=True)
            return 2

    print(f"[准备] {CENTER_SETTLE_S:.0f} 秒后把准心移到屏幕中心，然后开始测量"
          f"（圆形用例半径 {CIRCLE_RADIUS_PX}px，从屏幕边缘起步会被边界裁掉）", flush=True)
    original_cursor = cursor_position()
    time.sleep(CENTER_SETTLE_S)
    center_x, center_y = move_cursor_to_center()
    print(f"[准备] 准心已在屏幕中心 ({center_x},{center_y})，开始测量", flush=True)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    print(f"[连接] UDP 目标 {args.host}:{args.port}，观测源=本机光标；"
          f"判定：首动 < {FIRST_MOVE_LIMIT_MS:.1f}ms 且位移与预期一致", flush=True)
    repeat = max(1, args.repeat)
    print(f"[测量] 每类用例连续测量 {repeat} 轮，且每一轮都必须达标", flush=True)
    single_runs: list[dict] = []
    continuous_runs: list[dict] = []
    circle_runs: list[dict] = []
    circle_command_runs: list[list[Command]] = []
    single_plot: tuple[list[Sample], list[Command], int] | None = None
    continuous_plot: tuple[list[Sample], list[Command], int] | None = None
    circle_plot: tuple[list[Sample], list[Command], int] | None = None
    failures: list[str] = []
    sender = UdpSender(args.host, args.port)
    try:
        for attempt in range(1, repeat + 1):
            single, single_samples, single_commands, single_start = run_case(
                "single_20px", sender, 40_000, 0, 1)
            single_runs.append(single)
            if single_plot is None:
                # 图表与 CSV 取第一轮采样（足够代表波形）；每轮原始结果都进 summary.json。
                single_plot = (single_samples, single_commands, single_start)
            failures += judge_case(
                f"单次 {SINGLE_DISTANCE_PX}px（第 {attempt}/{repeat} 轮）",
                single, SINGLE_DISTANCE_PX)

            continuous, continuous_samples, continuous_commands, continuous_start = run_case(
                "three_20px_every_10ms", sender, 40_000, 10_000, CONTINUOUS_COMMANDS)
            continuous_runs.append(continuous)
            if continuous_plot is None:
                continuous_plot = (continuous_samples, continuous_commands, continuous_start)
            failures += judge_case(
                f"连续 {CONTINUOUS_COMMANDS} 次 {SINGLE_DISTANCE_PX}px"
                f"（间隔 10ms，第 {attempt}/{repeat} 轮）",
                continuous, SINGLE_DISTANCE_PX * CONTINUOUS_COMMANDS)

        # 长时移动用例放在最后：每 1 ms 一条、持续 10 s，画半径 100 px 的圆。
        circle_repeat = max(1, args.circle_repeat)
        for circle_attempt in range(1, circle_repeat + 1):
            print(f"[测量] 长时移动第 {circle_attempt}/{circle_repeat} 轮："
                  f"{CIRCLE_DURATION_S:.0f}s 每 1ms 一次画 r={CIRCLE_RADIUS_PX}px 圆"
                  f"（共 {int(round(CIRCLE_DURATION_S * 1_000_000 / CIRCLE_STEP_US))} 条命令）",
                  flush=True)
            circle, circle_samples, circle_commands, circle_start = run_circle_case(sender)
            circle_runs.append(circle)
            circle_command_runs.append(circle_commands)
            if circle_plot is None:
                circle_plot = (circle_samples, circle_commands, circle_start)
            print_circle_report(circle)
            failures += judge_circle(circle)
    finally:
        sender.close()

    single = single_runs[0]
    continuous = continuous_runs[0]
    circle = circle_runs[0]
    single_samples, single_commands, single_start = single_plot
    continuous_samples, continuous_commands, continuous_start = continuous_plot
    circle_samples, circle_commands, circle_start = circle_plot
    single_average = average_runs(single_runs)
    continuous_average = average_runs(continuous_runs)

    write_csv(args.output_dir / "single_20px.csv", single_samples)
    write_csv(args.output_dir / "three_20px_every_10ms.csv", continuous_samples)
    write_csv(args.output_dir / "circle_r100_10s.csv", circle_samples)
    circle_command_csvs = []
    for attempt, commands in enumerate(circle_command_runs, start=1):
        command_csv = f"circle_r100_10s_commands_run_{attempt:02d}.csv"
        write_command_csv(args.output_dir / command_csv, commands)
        circle_command_csvs.append(command_csv)
    svg_plot(args.output_dir / "single_20px.svg", "单次 20 px UDP 移动", single_samples, single_commands, single_start, single["analysis_start_us"], single["analysis_end_us"])
    svg_plot(args.output_dir / "three_20px_every_10ms.svg", "每 10 ms 发送一次 20 px，共 3 次", continuous_samples, continuous_commands, continuous_start, continuous["analysis_start_us"], continuous["analysis_end_us"])
    svg_plot(args.output_dir / "circle_r100_10s.svg",
             f"长时移动：{CIRCLE_DURATION_S:.0f}s 每 1ms 一次画 r={CIRCLE_RADIUS_PX}px 圆",
             circle_samples, circle_commands, circle_start,
             circle["analysis_start_us"], circle["analysis_end_us"])
    summary = {
        "target": f"{args.host}:{args.port}",
        "forced_no_precheck": bool(args.force),
        "criteria": {
            "first_move_limit_ms": FIRST_MOVE_LIMIT_MS,
            "single_expected_dx": SINGLE_DISTANCE_PX,
            "continuous_expected_dx": SINGLE_DISTANCE_PX * CONTINUOUS_COMMANDS,
            "repeat": repeat,
            "require_every_run_pass": True,
            "circle": {
                "radius_px": CIRCLE_RADIUS_PX,
                "duration_s": CIRCLE_DURATION_S,
                "step_us": CIRCLE_STEP_US,
                "step_px": CIRCLE_STEP_PX,
                "steps": int(round(CIRCLE_DURATION_S * 1_000_000 / CIRCLE_STEP_US)),
                "sample_us": CIRCLE_SAMPLE_US,
                "end_tolerance_px": CIRCLE_END_TOLERANCE_PX,
                "max_no_motion_gap_us": CIRCLE_MAX_GAP_US,
                "max_send_lag_us": CIRCLE_MAX_SEND_LAG_US,
                "repeat": circle_repeat,
            },
        },
        "judged": {
            "single_first_move_ms": first_move_latency_ms(single),
            "continuous_first_move_ms": first_move_latency_ms(continuous),
        },
        "average": {
            "single": single_average,
            "continuous": continuous_average,
        },
        "runs": {
            "single": single_runs,
            "continuous": continuous_runs,
            "circle": circle_runs,
        },
        "single": single,
        "continuous": continuous,
        "circle": circle,
        "circle_command_csvs": circle_command_csvs,
        "failures": failures,
        "failure_count": len(failures),
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.print_json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"图表、原始采样与判定已写入：{args.output_dir}"
          f"（summary.json / *.csv / *.svg；需要贴到终端时用 --print-json）")

    print("")
    print(f"===== {repeat} 轮平均结果 =====", flush=True)
    print_average(f"单次 {SINGLE_DISTANCE_PX}px", single_average)
    print_average(
        f"连续 {CONTINUOUS_COMMANDS} 次 {SINGLE_DISTANCE_PX}px（间隔 10ms）",
        continuous_average)

    print("")
    print(f"===== 长时移动检查（{circle_repeat} 轮，每轮 "
          f"{CIRCLE_DURATION_S:.0f}s / r={CIRCLE_RADIUS_PX}px / 每 1ms 一次）=====",
          flush=True)
    for index, run in enumerate(circle_runs, start=1):
        if circle_repeat > 1:
            print(f"  第 {index}/{circle_repeat} 轮：", flush=True)
        print_circle_report(run)

    # 收尾：把准心还给使用者原来的位置（测量前已挪到屏幕中心）。
    try:
        restore_cursor(original_cursor[0], original_cursor[1])
        print(f"[收尾] 准心已回到测量前的位置 ({original_cursor[0]},{original_cursor[1]})",
              flush=True)
    except RuntimeError as error:
        print(f"[收尾] 恢复准心失败：{error}", flush=True)

    if failures:
        print(f"[结论] FAIL（{repeat} 轮中累计 {len(failures)} 项未达标）", flush=True)
        for reason in failures:
            print(f"  - {reason}", flush=True)
        return 1
    print(f"[结论] PASS（{repeat} 轮全部达标：首次移动延迟 < {FIRST_MOVE_LIMIT_MS:.1f}ms "
          f"且位移与预期一致）", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
