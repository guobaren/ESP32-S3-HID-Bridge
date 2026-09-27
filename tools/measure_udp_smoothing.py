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

默认执行一次 20px 和三次连续 20px（命令间隔 10ms）。50% 位移耗时、逐命令延迟、
每步位移等只作为过程信息写入 JSON/CSV/SVG，不参与判定。
"""

from __future__ import annotations

import argparse
import ctypes
import csv
import json
import socket
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

# 判定阈值与前置检查标识。
FIRST_MOVE_LIMIT_MS = 10.0
# 克隆设备型号随被代理的鼠标变化：046D:C092（有线 G102）与 046D:C539（Lightspeed
# 接收器）都在实机出现过。因此前置检查默认按“当前枚举到的任意 USB 罗技节点”判定，
# 需要精确型号时用 --expect-vidpid 046D:xxxx。
CLONE_VENDOR_HINT = "VID_046D"
CLONE_USB_PREFIX = "USB\\VID_046D&PID_"
CLONE_INSTANCE_HINT = "VID_046D&PID_C092"  # 历史样本，仅用于文案提示
SINGLE_DISTANCE_PX = 20
CONTINUOUS_COMMANDS = 3


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


_USER32 = ctypes.WinDLL("user32", use_last_error=True)
_USER32.GetCursorPos.argtypes = [ctypes.POINTER(Point)]
_USER32.GetCursorPos.restype = ctypes.c_bool
_USER32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
_USER32.SetCursorPos.restype = ctypes.c_bool


@dataclass(frozen=True)
class Sample:
    elapsed_us: int
    x: int
    y: int


@dataclass(frozen=True)
class Command:
    index: int
    sent_us: int
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


class UdpSender:
    def __init__(self, host: str, port: int) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._address = (host, port)

    def send(self, dx: int, dy: int) -> int:
        payload = json.dumps(
            {"dx": int(dx), "dy": int(dy), "wheel": 0, "pan": 0},
            separators=(",", ":"),
        ).encode("utf-8")
        return self._socket.sendto(payload, self._address)

    def close(self) -> None:
        self._socket.close()


def send_command(sender: UdpSender, start_ns: int, commands: list[Command], dx: int, dy: int) -> None:
    sent_us = (time.perf_counter_ns() - start_ns) // 1_000
    sender.send(dx, dy)
    commands.append(Command(len(commands) + 1, sent_us, dx, dy))


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
                "sent_us": command.sent_us,
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
    commands = [Command(command.index, command.sent_us - start_us, command.dx, command.dy) for command in commands]
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


def run_case(name: str, sender: UdpSender, settle_us: int, interval_us: int, count: int) -> tuple[dict, list[Sample], list[Command], int]:
    start_x, start_y = cursor_position()
    commands: list[Command] = []
    start_ns = time.perf_counter_ns()
    send_deadlines_us = [settle_us + index * interval_us for index in range(count)]
    end_us = settle_us + (count - 1) * interval_us + 80_000
    next_sample_us = 0
    samples: list[Sample] = []
    next_command = 0
    while next_sample_us <= end_us:
        now_us = (time.perf_counter_ns() - start_ns) // 1_000
        while next_command < count and now_us >= send_deadlines_us[next_command]:
            send_command(sender, start_ns, commands, 20, 0)
            next_command += 1
        if now_us >= next_sample_us:
            x, y = cursor_position()
            samples.append(Sample(now_us, x, y))
            next_sample_us += 1_000
        # 采样和发送调度路径禁止 sleep：Windows 的线程睡眠粒度可能约为 15.6 ms，
        # 会直接跳过多个 1 ms 桶。这里使用 perf_counter_ns() 忙等到下一个绝对时间点。
    if commands:
        sender.send(-20 * count, 0)
        time.sleep(0.08)
    restore_cursor(start_x, start_y)
    end_x, end_y = cursor_position()
    targets = [start_x + 20 * index for index in range(1, count + 1)]
    result = command_metrics(samples, commands, start_x, targets, start_x + 20 * count)
    result.update({"name": name, "start": [start_x, start_y], "end": [end_x, end_y], "end_restored": [end_x, end_y] == [start_x, start_y]})
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
    parser.add_argument("--print-json", action="store_true",
                        help="额外在标准输出打印完整判定 JSON（缺省只写入 summary.json）")
    args = parser.parse_args()

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

    args.output_dir.mkdir(parents=True, exist_ok=True)
    print(f"[连接] UDP 目标 {args.host}:{args.port}，观测源=本机光标；"
          f"判定：首动 < {FIRST_MOVE_LIMIT_MS:.1f}ms 且位移与预期一致", flush=True)
    sender = UdpSender(args.host, args.port)
    try:
        single, single_samples, single_commands, single_start = run_case("single_20px", sender, 40_000, 0, 1)
        continuous, continuous_samples, continuous_commands, continuous_start = run_case(
            "three_20px_every_10ms", sender, 40_000, 10_000, CONTINUOUS_COMMANDS)
    finally:
        sender.close()

    failures: list[str] = []
    failures += judge_case(f"单次 {SINGLE_DISTANCE_PX}px", single,
                           SINGLE_DISTANCE_PX)
    failures += judge_case(
        f"连续 {CONTINUOUS_COMMANDS} 次 {SINGLE_DISTANCE_PX}px（间隔 10ms）", continuous,
        SINGLE_DISTANCE_PX * CONTINUOUS_COMMANDS)

    write_csv(args.output_dir / "single_20px.csv", single_samples)
    write_csv(args.output_dir / "three_20px_every_10ms.csv", continuous_samples)
    svg_plot(args.output_dir / "single_20px.svg", "单次 20 px UDP 移动", single_samples, single_commands, single_start, single["analysis_start_us"], single["analysis_end_us"])
    svg_plot(args.output_dir / "three_20px_every_10ms.svg", "每 10 ms 发送一次 20 px，共 3 次", continuous_samples, continuous_commands, continuous_start, continuous["analysis_start_us"], continuous["analysis_end_us"])
    summary = {
        "target": f"{args.host}:{args.port}",
        "criteria": {
            "first_move_limit_ms": FIRST_MOVE_LIMIT_MS,
            "single_expected_dx": SINGLE_DISTANCE_PX,
            "continuous_expected_dx": SINGLE_DISTANCE_PX * CONTINUOUS_COMMANDS,
        },
        "judged": {
            "single_first_move_ms": first_move_latency_ms(single),
            "continuous_first_move_ms": first_move_latency_ms(continuous),
        },
        "single": single,
        "continuous": continuous,
        "failures": failures,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.print_json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"图表、原始采样与判定已写入：{args.output_dir}"
          f"（summary.json / *.csv / *.svg；需要贴到终端时用 --print-json）")
    if failures:
        print("[结论] FAIL", flush=True)
        for reason in failures:
            print(f"  - {reason}", flush=True)
        return 1
    print("[结论] PASS（首次移动延迟达标且位移与预期一致）", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
