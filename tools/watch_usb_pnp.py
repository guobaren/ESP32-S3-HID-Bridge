"""观察两板在这台电脑上呈现的 USB 设备变化（角色探测与克隆枚举）。

用于 O1–O6 线序诊断：电脑侧板(P)在身份未定时每 3 秒轮换 USB Device/Host，
Device 探测窗口会把 **默认探测设备** `VID_303A&PID_4005` 挂到目标电脑上；
锁定 `PC_DEVICE` 后该设备被撤下，克隆安装成功后出现 `VID_046D&PID_C092`。

因此只看这台电脑的 PnP 就能区分：

* 反复出现/消失（约 3 秒周期）→ P 在轮换、始终拿不到身份；
* 出现一次后长期消失，之后出现 C092 → P 已锁定并完成克隆；
* 从不出现 → P 的 Device 探测没有被这台电脑枚举（或 P 一直在 Host 模式）。

用法::

    python .\\tools\\watch_usb_pnp.py --seconds 180

输出：`artifacts/tests/pnp-watch-<时间戳>.log`，只在设备集合变化时写一行，
每 10 秒写一行心跳。不改动任何设备，只读查询。
"""

from __future__ import annotations

import argparse
import datetime
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_WATCH = ("VID_303A&PID_4005", "VID_046D&PID_C092")
HEARTBEAT_SECONDS = 10.0


def query_present(patterns: tuple[str, ...]) -> list[str] | None:
    """返回当前在位的匹配 InstanceId 列表；查询失败返回 None。"""
    conditions = " -or ".join(
        f"$_.InstanceId -like '*{pattern}*'" for pattern in patterns)
    command = (
        "Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
        f"Where-Object {{ {conditions} }} | "
        "Select-Object -ExpandProperty InstanceId"
    )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return sorted(line.strip() for line in completed.stdout.splitlines() if line.strip())


def summarize(instance_ids: list[str]) -> str:
    """把 InstanceId 列表压成 (设备ID x 数量) 的可读串。"""
    counts: dict[str, int] = {}
    for instance_id in instance_ids:
        parts = instance_id.split("\\")
        key = parts[1] if len(parts) > 1 else instance_id
        counts[key] = counts.get(key, 0) + 1
    if not counts:
        return "（无匹配设备）"
    return " ".join(f"{key}x{count}" for key, count in sorted(counts.items()))


def main() -> int:
    parser = argparse.ArgumentParser(description="观察两板在目标电脑上的 USB 设备变化")
    parser.add_argument("--seconds", type=float, default=180.0)
    parser.add_argument("--interval", type=float, default=0.6)
    parser.add_argument("--outdir", default="artifacts/tests")
    parser.add_argument("--watch", default=",".join(DEFAULT_WATCH),
                        help="逗号分隔的 InstanceId 子串，默认探测设备与克隆设备")
    parser.add_argument("--tag", default="")
    args = parser.parse_args()

    patterns = tuple(item.strip() for item in args.watch.split(",") if item.strip())
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    suffix = f"-{args.tag}" if args.tag else ""
    path = outdir / f"pnp-watch-{stamp}{suffix}.log"

    start = time.monotonic()
    previous: list[str] | None = []
    last_heartbeat = start
    print(f"开始观察 {', '.join(patterns)}，时长 {args.seconds:.0f}s -> {path}", flush=True)

    with path.open("w", encoding="utf-8") as sink:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed > args.seconds:
                break
            current = query_present(patterns)
            if current is None:
                sink.write(f"[+{elapsed:7.2f}s] [[PnP 查询失败]]\n")
                sink.flush()
            elif current != previous:
                sink.write(f"[+{elapsed:7.2f}s] {summarize(current)}\n")
                sink.flush()
                print(f"[+{elapsed:6.2f}s] {summarize(current)}", flush=True)
                previous = current
                last_heartbeat = now
            elif now - last_heartbeat >= HEARTBEAT_SECONDS:
                sink.write(f"[+{elapsed:7.2f}s] （心跳）{summarize(current)}\n")
                sink.flush()
                last_heartbeat = now
            time.sleep(args.interval)

        elapsed = time.monotonic() - start
        sink.write(f"[+{elapsed:7.2f}s] 观察结束：{summarize(previous or [])}\n")
    print(f"观察结束 -> {path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
