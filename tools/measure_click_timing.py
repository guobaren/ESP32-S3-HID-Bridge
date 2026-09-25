"""从电脑侧测量鼠标按键（左键）事件的时间特性，用于量化"点击延迟"。

原理：以高频率（约 40 万 Hz，配合 timeBeginPeriod(1) + 忙等）采样左键状态，
记录每一次“按下→抬起”的持续时长与相邻点击的间隔。

判读：
  - 轻点正常持续约 60~120 ms；若明显更长/更短，说明链路把事件拉伸或吞并；
  - 以固定节律点击时，间隔方差就是链路抖动；
  - 出现“一次点击被拆成两次”或“按下后长时间不抬”说明事件被延迟/重复投递。

用法：
    python tools/measure_click_timing.py --seconds 30
"""

from __future__ import annotations

import argparse
import ctypes
import statistics
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
winmm = ctypes.WinDLL("winmm", use_last_error=True)
VK_LBUTTON = 0x01
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short


def main() -> int:
    parser = argparse.ArgumentParser(description="鼠标左键点击时间特性测量")
    parser.add_argument("--seconds", type=float, default=30.0, help="测量时长（秒）")
    args = parser.parse_args()

    winmm.timeBeginPeriod(1)
    events: list[tuple[str, float]] = []
    samples = 0
    previous = (user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
    started = time.perf_counter()
    deadline = started + args.seconds
    while time.perf_counter() < deadline:
        pressed = (user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
        samples += 1
        if pressed != previous:
            # 同时记录墙钟时间：用于与板子串口日志（抓包工具带墙钟）配对
            events.append(("按下" if pressed else "抬起",
                           time.perf_counter() - started, time.time()))
            previous = pressed
    elapsed = time.perf_counter() - started
    winmm.timeEndPeriod(1)

    print(f"测量时长      : {elapsed:.2f} s")
    print(f"采样自检      : {samples} 次 → {samples / elapsed:.0f} Hz"
          + ("（合格）" if samples / elapsed >= 500 else "（不足，结论不可用）"))
    if samples / elapsed < 500:
        return 3
    if not events:
        print("未检测到任何按键事件——测量期间请点击鼠标左键")
        return 2

    clicks: list[tuple[float, float, float]] = []   # (按下时刻, 持续时长, 墙钟)
    press_time: float | None = None
    press_wall = 0.0
    for kind, when, wall in events:
        if kind == "按下":
            if press_time is not None:
                print(f"警告：{when*1000:.0f} ms 处出现“按下”但前一次尚未抬起（事件被重复/丢失）")
            press_time = when
            press_wall = wall
        elif press_time is not None:
            clicks.append((press_time, (when - press_time) * 1000.0, press_wall))
            press_time = None
    if press_time is not None:
        print(f"注意：测量结束时左键仍处于按下状态（{press_time*1000:.0f} ms 起）")

    print(f"点击次数      : {len(clicks)}")
    if clicks:
        durations = [d for _, d, _ in clicks]
        print(f"点击时长      : p50={statistics.median(durations):.1f} ms  "
              f"min={min(durations):.1f}  max={max(durations):.1f} ms")
        intervals = [clicks[i][0] - clicks[i - 1][0] for i in range(1, len(clicks))]
        if intervals:
            mean = statistics.fmean(intervals)
            print(f"点击间隔      : p50={statistics.median(intervals)*1000:.0f} ms  "
                  f"min={min(intervals)*1000:.0f}  max={max(intervals)*1000:.0f} ms")
            if len(intervals) > 2:
                print(f"  间隔标准差  : {statistics.pstdev(intervals)*1000:.0f} ms"
                      f"（你以固定节律点击时，这个值就是链路抖动）")
        print("逐次明细      :")
        for index, (when, duration, wall) in enumerate(clicks, 1):
            print(f"  #{index:2d} 相对 {when*1000:8.1f} ms，墙钟 {wall:.3f}，持续 {duration:6.1f} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
