"""测量远程 UDP 鼠标输入的延迟、5 槽平滑和连续命令重叠效果。

脚本在本机每 1 ms 时间桶采样一次鼠标坐标，向远端 HidBridge UDP 端口发送
相对位移，并生成 CSV、JSON 和两个 SVG 图。测试结束后用反向 UDP 命令恢复起点。
"""

from __future__ import annotations

import argparse
import ctypes
import csv
import json
import socket
import threading
import time
from dataclasses import asdict, dataclass
from pathlib import Path


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
    if not _USER32.SetCursorPos(x, y):
        raise ctypes.WinError(ctypes.get_last_error())


class Sampler:
    def __init__(self, interval_us: int = 1_000) -> None:
        self.interval_us = interval_us
        self._samples: list[Sample] = []
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._start_ns = 0

    @property
    def samples(self) -> list[Sample]:
        return list(self._samples)

    def start(self) -> None:
        self._start_ns = time.perf_counter_ns()
        self._thread = threading.Thread(target=self._run, name="cursor-1ms-sampler", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
            if self._thread.is_alive():
                raise RuntimeError("鼠标采样线程未在 2 秒内退出")

    def elapsed_us(self) -> int:
        return (time.perf_counter_ns() - self._start_ns) // 1_000

    def _run(self) -> None:
        next_us = 0
        while not self._stop.is_set():
            now_us = self.elapsed_us()
            if now_us < next_us:
                remaining_us = next_us - now_us
                if remaining_us > 200:
                    time.sleep((remaining_us - 100) / 1_000_000)
                continue
            x, y = cursor_position()
            self._samples.append(Sample(now_us, x, y))
            next_us += self.interval_us


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


def wait_us(sampler: Sampler, duration_us: int) -> None:
    deadline = sampler.elapsed_us() + duration_us
    while sampler.elapsed_us() < deadline:
        time.sleep(0.0002)


def send_command(sender: UdpSender, sampler: Sampler, commands: list[Command], dx: int, dy: int) -> None:
    sent_us = sampler.elapsed_us()
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
    sampler = Sampler()
    commands: list[Command] = []
    sampler.start()
    try:
        wait_us(sampler, settle_us)
        for _ in range(count):
            send_command(sender, sampler, commands, 20, 0)
            if count > 1:
                wait_us(sampler, interval_us)
        wait_us(sampler, 100_000)
        samples = sampler.samples
    finally:
        sampler.stop()
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.3.50")
    parser.add_argument("--port", type=int, default=24814)
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/tests/udp-smoothing-measurement"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    sender = UdpSender(args.host, args.port)
    try:
        single, single_samples, single_commands, single_start = run_case("single_20px", sender, 40_000, 0, 1)
        continuous, continuous_samples, continuous_commands, continuous_start = run_case("three_20px_every_10ms", sender, 40_000, 10_000, 3)
    finally:
        sender.close()
    write_csv(args.output_dir / "single_20px.csv", single_samples)
    write_csv(args.output_dir / "three_20px_every_10ms.csv", continuous_samples)
    svg_plot(args.output_dir / "single_20px.svg", "单次 20 px UDP 移动", single_samples, single_commands, single_start, single["analysis_start_us"], single["analysis_end_us"])
    svg_plot(args.output_dir / "three_20px_every_10ms.svg", "每 10 ms 发送一次 20 px，共 3 次", continuous_samples, continuous_commands, continuous_start, continuous["analysis_start_us"], continuous["analysis_end_us"])
    summary = {"target": f"{args.host}:{args.port}", "single": single, "continuous": continuous}
    (args.output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"图表和原始采样已写入：{args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
