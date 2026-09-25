"""``capture_window_winapi.restore_window_to_normal`` 的自测（不需要 G HUB）。

用一个临时记事本窗口验证三条契约：

1. 最小化 → 还原后不是最小化、``showCmd`` 为正常尺寸（``1``），矩形落在可见桌面内；
2. 先最大化再最小化 → 还原后回到**正常尺寸**，不得直接回到最大化；
3. 已经可见且未最小化的窗口 → 调用还原没有任何副作用（不改矩形、不变最大化）。

用法::

    python .\\tools\\selftest_capture_restore.py

退出码 0 表示三项契约全部满足。测试结束会关闭自己启动的记事本进程，不触碰
Logitech G HUB 窗口。
"""

from __future__ import annotations

import ctypes
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_window_winapi as cw  # noqa: E402

SW_MINIMIZE = 6
SW_MAXIMIZE = 3
SHOW_NORMAL = 1
SHOW_MAXIMIZED = 3


def _placement(api: cw.WinApi, hwnd: int) -> cw.WINDOWPLACEMENT:
    placement = cw.WINDOWPLACEMENT()
    placement.length = ctypes.sizeof(cw.WINDOWPLACEMENT)
    api.user32.GetWindowPlacement(hwnd, ctypes.byref(placement))
    return placement


def _rect(api: cw.WinApi, hwnd: int) -> cw.RECT:
    rect = cw.RECT()
    api.user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect


def _inside_virtual_screen(api: cw.WinApi, rect: cw.RECT) -> bool:
    left, top, width, height = cw._virtual_screen(api)
    return (rect.right > left and rect.left < left + width and
            rect.bottom > top and rect.top < top + height)


def main() -> int:
    api = cw.WinApi()
    user32 = api.user32
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowW.restype = wintypes.HWND

    process = subprocess.Popen(["notepad.exe"])
    failures: list[str] = []
    try:
        hwnd = 0
        for _ in range(40):
            hwnd = user32.FindWindowW("Notepad", None)
            if hwnd:
                break
            time.sleep(0.25)
        if not hwnd:
            print("FAIL: 未能启动记事本窗口，自测无效")
            return 1
        time.sleep(0.5)

        # 用例 1：最小化 → 还原
        user32.ShowWindow(hwnd, SW_MINIMIZE)
        time.sleep(0.4)
        minimized = bool(user32.IsIconic(hwnd))
        restored = cw.restore_window_to_normal(api, hwnd)
        time.sleep(0.3)
        placement = _placement(api, hwnd)
        rect = _rect(api, hwnd)
        print(f"[用例1] 最小化前={minimized} 还原返回={restored} "
              f"仍最小化={bool(user32.IsIconic(hwnd))} showCmd={placement.showCmd} "
              f"可见桌面内={_inside_virtual_screen(api, rect)}")
        if not minimized:
            failures.append("用例1未能进入最小化状态，自测无效")
        if not restored or user32.IsIconic(hwnd):
            failures.append("用例1：窗口仍处于最小化")
        if placement.showCmd == SHOW_MAXIMIZED:
            failures.append("用例1：还原后变成了最大化")
        if not _inside_virtual_screen(api, rect):
            failures.append("用例1：还原后窗口仍在可见桌面之外")

        # 用例 2：最大化后最小化 → 还原必须回到正常尺寸
        user32.ShowWindow(hwnd, SW_MAXIMIZE)
        time.sleep(0.4)
        user32.ShowWindow(hwnd, SW_MINIMIZE)
        time.sleep(0.4)
        restored_again = cw.restore_window_to_normal(api, hwnd)
        time.sleep(0.3)
        placement_again = _placement(api, hwnd)
        print(f"[用例2] 最大化后最小化→还原：返回={restored_again} "
              f"showCmd={placement_again.showCmd} 仍最小化={bool(user32.IsIconic(hwnd))}")
        if not restored_again:
            failures.append("用例2：还原函数返回失败")
        if placement_again.showCmd == SHOW_MAXIMIZED:
            failures.append("用例2：还原后回到了最大化（要求不得直接到最大化）")
        elif placement_again.showCmd != SHOW_NORMAL:
            failures.append(
                f"用例2：还原后 showCmd={placement_again.showCmd}，期望 {SHOW_NORMAL}")
        if user32.IsIconic(hwnd):
            failures.append("用例2：还原后仍处于最小化")

        # 用例 3：已可见窗口调用还原 = 无副作用
        before = _rect(api, hwnd)
        unchanged_result = cw.restore_window_to_normal(api, hwnd)
        time.sleep(0.2)
        after = _rect(api, hwnd)
        same = (before.left, before.top, before.right, before.bottom) == \
               (after.left, after.top, after.right, after.bottom)
        final_placement = _placement(api, hwnd)
        print(f"[用例3] 已可见窗口：返回={unchanged_result} 矩形不变={same} "
              f"showCmd={final_placement.showCmd}")
        if not unchanged_result or not same:
            failures.append("用例3：对已可见窗口产生了改动")
        if final_placement.showCmd == SHOW_MAXIMIZED:
            failures.append("用例3：把已可见窗口变成了最大化")

        if failures:
            print("[结论] FAIL")
            for reason in failures:
                print(f"  - {reason}")
            return 1
        print("[结论] PASS（最小化可还原、不会最大化、已可见窗口无副作用）")
        return 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
