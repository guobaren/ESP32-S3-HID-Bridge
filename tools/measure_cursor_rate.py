"""从电脑侧测量鼠标的有效更新率与卡顿分布（不依赖板子串口）。

原理：以约 1 kHz 采样 GetCursorPos，记录"位置发生变化"的时刻间隔。
Windows 会把多次 HID 报告合并成一次光标位移，因此这个间隔分布反映的是
**主机实际看到的移动刷新节奏**，也就是用户感知到的延迟/卡顿。

用法：
    python tools/measure_cursor_rate.py --seconds 8

输出：
    更新次数、有效频率、间隔分位数（p50/p90/p99/max）、>20ms 的长间隔清单。
    判读：正常 1 kHz 鼠标应表现为高更新频率 + 长间隔极少；
         若出现"每约 1 秒一次 3~5 个更新"的簇，会在长间隔清单里看到 1 s 级空档。
"""

from __future__ import annotations

import argparse
import ctypes
import statistics
import time
from ctypes import wintypes


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetCursorPos.argtypes = [ctypes.POINTER(POINT)]
user32.GetCursorPos.restype = wintypes.BOOL
winmm = ctypes.WinDLL("winmm", use_last_error=True)
# 左键状态用作"鼠标已抬起"的标记：悬空期间任何光标位移都只能来自链路（残留）。
VK_LBUTTON = 0x01
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short

# 关键：Windows 默认定时器精度约 15.6 ms，不提升的话 time.sleep(0.0005) 实际会睡
# 15.6 ms，测出来的"间隔"全是采样周期本身（我第一版就踩了这个坑）。
TIMER_RESOLUTION_MS = 1


def main() -> int:
    parser = argparse.ArgumentParser(description="光标更新率与卡顿测量")
    parser.add_argument("--seconds", type=float, default=8.0, help="测量时长（秒）")
    parser.add_argument("--save-csv", default="",
                        help="把时间线存成 CSV（时刻ms,dx,dy）供独立分析")
    parser.add_argument("--long-ms", type=float, default=20.0,
                        help="超过该毫秒数记为一次长间隔")
    args = parser.parse_args()

    winmm.timeBeginPeriod(TIMER_RESOLUTION_MS)
    point = POINT()
    user32.GetCursorPos(ctypes.byref(point))
    last_x, last_y = point.x, point.y
    last_change = time.perf_counter()

    changes = 0
    samples = 0
    gaps: list[float] = []
    timeline: list[tuple[float, int, int]] = []
    lifted: list[tuple[float, float]] = []      # (开始, 结束) 左键按住区间
    lift_start: float | None = None
    started = time.perf_counter()
    deadline = started + args.seconds
    while time.perf_counter() < deadline:
        user32.GetCursorPos(ctypes.byref(point))
        samples += 1
        pressed = (user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
        if pressed and lift_start is None:
            lift_start = time.perf_counter() - started
        elif not pressed and lift_start is not None:
            lifted.append((lift_start, time.perf_counter() - started))
            lift_start = None
        if point.x != last_x or point.y != last_y:
            now = time.perf_counter()
            gaps.append((now - last_change) * 1000.0)
            timeline.append((now - started, point.x - last_x, point.y - last_y))
            last_change = now
            last_x, last_y = point.x, point.y
            changes += 1
        # 忙等采样：不依赖 sleep 精度，保证 kHz 级采样
    elapsed = time.perf_counter() - started
    winmm.timeEndPeriod(TIMER_RESOLUTION_MS)

    if changes < 2:
        print(f"更新次数过少（{changes}）——测量期间鼠标几乎没动，请重新测量")
        return 2

    ordered = sorted(gaps)
    def pct(p: float) -> float:
        index = min(len(ordered) - 1, max(0, int(round(p / 100.0 * (len(ordered) - 1)))))
        return ordered[index]

    print(f"测量时长      : {elapsed:.2f} s")
    print(f"采样自检      : {samples} 次采样 → {samples / elapsed:.0f} Hz"
          f"（低于 500 Hz 说明采样精度不足，结论不可用）")
    if samples / elapsed < 500:
        print("判定：采样率不足，本次结论不可用")
        return 3
    print(f"位置更新次数  : {changes}  →  有效更新率 ≈ {changes / args.seconds:.0f} Hz")
    print(f"间隔 p50/p90  : {pct(50):.2f} ms / {pct(90):.2f} ms")
    print(f"间隔 p99/max  : {pct(99):.2f} ms / {ordered[-1]:.2f} ms")
    long_gaps = [g for g in gaps if g >= args.long_ms]
    print(f"长间隔(≥{args.long_ms:.0f}ms): {len(long_gaps)} 次"
          + (f"，合计 {sum(long_gaps):.0f} ms" if long_gaps else ""))
    if long_gaps:
        shown = ", ".join(f"{g:.0f}" for g in sorted(long_gaps, reverse=True)[:12])
        print(f"  最长几次    : {shown} ms")
    # 按“每次甩动”分别分析尾迹：先按位移大小找出甩动段，再看每段结束后 300 ms 内
    # 是否还有小位移（这才是用户说的“停手后又动几次”）。
    if timeline:
        sizes = [abs(dx) + abs(dy) for _, dx, dy in timeline]
        peak = max(sizes)
        big_threshold = max(20, peak * 0.25)
        bursts: list[list[int]] = []
        for index, size in enumerate(sizes):
            if size < big_threshold:
                continue
            if bursts and timeline[index][0] - timeline[bursts[-1][-1]][0] < 0.05:
                bursts[-1].append(index)
            else:
                bursts.append([index])
        print(f"最大单次位移  : {peak} 计数；识别到 {len(bursts)} 次甩动"
              f"（阈值 {big_threshold:.0f} 计数）")
        for order, burst in enumerate(bursts, 1):
            end_time = timeline[burst[-1]][0]
            residuals = [(dt, dx, dy) for dt, dx, dy in timeline
                         if end_time < dt <= end_time + 0.3
                         and abs(dx) + abs(dy) < big_threshold]
            if not residuals:
                print(f"  甩动{order}: 停手后 300 ms 内无残余位移")
                continue
            span = (residuals[-1][0] - end_time) * 1000.0
            total = sum(abs(dx) + abs(dy) for _, dx, dy in residuals)
            mags = [abs(dx) + abs(dy) for _, dx, dy in residuals]
            print(f"  甩动{order}: 停手后残余 {len(residuals)} 次 / 持续 {span:.0f} ms"
                  f" / 累计 {total} 计数 / 单次 1~{max(mags)} 计数")
            head = "  ".join(f"{(dt-end_time)*1000:.1f}ms:{abs(dx)+abs(dy)}"
                             for dt, dx, dy in residuals[:10])
            print(f"      前几次(时刻:位移) {head}")

    if lift_start is not None:
        lifted.append((lift_start, elapsed))

    # 悬空区间（左键按住）内的位移：这是"链路残留"的判定依据
    if lifted:
        print(f"悬空标记      : 检测到 {len(lifted)} 段“左键按住”（视为鼠标已抬起）")
        for order, (begin, end) in enumerate(lifted, 1):
            inside = [(dt, dx, dy) for dt, dx, dy in timeline if begin <= dt <= end]
            total = sum(abs(dx) + abs(dy) for _, dx, dy in inside)
            print(f"  段{order}（{(end-begin)*1000:.0f} ms）: 期间位移 {len(inside)} 次 / 累计 {total} 计数"
                  + ("  ← 悬空仍有位移，属链路残留" if total > 0 else "  ← 悬空零位移（正常）"))
            if inside:
                head = "  ".join(f"{(dt-begin)*1000:.1f}ms:{abs(dx)+abs(dy)}"
                                 for dt, dx, dy in inside[:10])
                print(f"      明细 {head}")
    else:
        print("悬空标记      : 未检测到左键按住区间（本次无法判定链路残留）")

    if args.save_csv and timeline:
        with open(args.save_csv, "w", encoding="utf-8") as handle:
            handle.write("t_ms,dx,dy\n")
            for dt, dx, dy in timeline:
                handle.write(f"{dt*1000:.3f},{dx},{dy}\n")
        print(f"原始时间线已存: {args.save_csv}（{len(timeline)} 行）")

    clusters = {}
    for g in long_gaps:
        bucket = round(g / 100.0) * 100
        clusters[bucket] = clusters.get(bucket, 0) + 1
    if clusters:
        print("  长间隔分布  : " + ", ".join(f"{k}ms×{v}" for k, v in sorted(clusters.items())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
