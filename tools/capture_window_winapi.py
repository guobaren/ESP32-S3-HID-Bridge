#!/usr/bin/env python3
"""置顶并截图 Logitech G HUB 顶层窗口。"""

from __future__ import annotations

import argparse
import ctypes
import ntpath
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from ctypes import wintypes

from PIL import Image, ImageStat


PW_RENDERFULLCONTENT = 0x00000002
HWND_TOPMOST_VALUE = -1
HWND_TOP_VALUE = 0
SW_RESTORE = 9
SW_SHOWNOACTIVATE = 4
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
SWP_NOZORDER = 0x0004
SW_SHOWMINIMIZED = 2
SW_SHOWMAXIMIZED = 3
SW_SHOWNORMAL = 1
MIN_RESTORED_WIDTH = 320
MIN_RESTORED_HEIGHT = 240
EDGE_MARGIN = 8
SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
BI_RGB = 0
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79
MAX_BITMAP_DIMENSION = 16384
MAX_BITMAP_PIXELS = 100_000_000


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class WINDOWPLACEMENT(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("showCmd", ctypes.c_uint),
        ("ptMinPosition", POINT),
        ("ptMaxPosition", POINT),
        ("rcNormalPosition", RECT),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_uint32),
        ("biWidth", ctypes.c_int32),
        ("biHeight", ctypes.c_int32),
        ("biPlanes", ctypes.c_uint16),
        ("biBitCount", ctypes.c_uint16),
        ("biCompression", ctypes.c_uint32),
        ("biSizeImage", ctypes.c_uint32),
        ("biXPelsPerMeter", ctypes.c_int32),
        ("biYPelsPerMeter", ctypes.c_int32),
        ("biClrUsed", ctypes.c_uint32),
        ("biClrImportant", ctypes.c_uint32),
    ]


class RGBQUAD(ctypes.Structure):
    _fields_ = [
        ("rgbBlue", ctypes.c_ubyte),
        ("rgbGreen", ctypes.c_ubyte),
        ("rgbRed", ctypes.c_ubyte),
        ("rgbReserved", ctypes.c_ubyte),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", RGBQUAD * 1)]


@dataclass(frozen=True)
class WindowInfo:
    hwnd: int
    pid: int
    process_path: str
    title: str
    visible: bool

    @property
    def process_name(self) -> str:
        return ntpath.basename(self.process_path)


class WinApi:
    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("此脚本只能在 Windows 上运行。")

        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self._set_signatures()

    def _set_signatures(self) -> None:
        w = wintypes

        self.enum_windows_callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.user32.EnumWindows.argtypes = [self.enum_windows_callback_type, w.LPARAM]
        self.user32.EnumWindows.restype = w.BOOL
        self.user32.GetWindowTextLengthW.argtypes = [w.HWND]
        self.user32.GetWindowTextLengthW.restype = ctypes.c_int
        self.user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        self.user32.GetWindowTextW.restype = ctypes.c_int
        self.user32.IsWindow.argtypes = [w.HWND]
        self.user32.IsWindow.restype = w.BOOL
        self.user32.IsWindowVisible.argtypes = [w.HWND]
        self.user32.IsWindowVisible.restype = w.BOOL
        self.user32.GetWindowThreadProcessId.argtypes = [w.HWND, w.LPDWORD]
        self.user32.GetWindowThreadProcessId.restype = w.DWORD
        self.user32.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(RECT)]
        self.user32.GetWindowRect.restype = w.BOOL
        self.user32.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
        self.user32.SetWindowPos.restype = w.BOOL
        self.user32.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
        self.user32.ShowWindow.restype = w.BOOL
        self.user32.IsIconic.argtypes = [w.HWND]
        self.user32.IsIconic.restype = w.BOOL
        self.user32.ShowWindowAsync.argtypes = [w.HWND, ctypes.c_int]
        self.user32.ShowWindowAsync.restype = w.BOOL
        self.user32.GetWindowPlacement.argtypes = [w.HWND, ctypes.POINTER(WINDOWPLACEMENT)]
        self.user32.GetWindowPlacement.restype = w.BOOL
        self.user32.SetWindowPlacement.argtypes = [w.HWND, ctypes.POINTER(WINDOWPLACEMENT)]
        self.user32.SetWindowPlacement.restype = w.BOOL
        self.user32.SetForegroundWindow.argtypes = [w.HWND]
        self.user32.SetForegroundWindow.restype = w.BOOL
        self.user32.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
        self.user32.PrintWindow.restype = w.BOOL
        self.user32.GetDC.argtypes = [w.HWND]
        self.user32.GetDC.restype = w.HDC
        self.user32.ReleaseDC.argtypes = [w.HWND, w.HDC]
        self.user32.ReleaseDC.restype = ctypes.c_int
        self.user32.GetSystemMetrics.argtypes = [ctypes.c_int]
        self.user32.GetSystemMetrics.restype = ctypes.c_int

        self.kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.kernel32.OpenProcess.restype = w.HANDLE
        self.kernel32.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, w.LPDWORD]
        self.kernel32.QueryFullProcessImageNameW.restype = w.BOOL
        self.kernel32.CloseHandle.argtypes = [w.HANDLE]
        self.kernel32.CloseHandle.restype = w.BOOL

        self.gdi32.CreateCompatibleDC.argtypes = [w.HDC]
        self.gdi32.CreateCompatibleDC.restype = w.HDC
        self.gdi32.DeleteDC.argtypes = [w.HDC]
        self.gdi32.DeleteDC.restype = w.BOOL
        self.gdi32.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
        self.gdi32.SelectObject.restype = w.HGDIOBJ
        self.gdi32.DeleteObject.argtypes = [w.HGDIOBJ]
        self.gdi32.DeleteObject.restype = w.BOOL
        self.gdi32.CreateDIBSection.argtypes = [
            w.HDC,
            ctypes.POINTER(BITMAPINFO),
            w.UINT,
            ctypes.POINTER(ctypes.c_void_p),
            w.HANDLE,
            w.DWORD,
        ]
        self.gdi32.CreateDIBSection.restype = w.HBITMAP
        self.gdi32.BitBlt.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.HDC, ctypes.c_int, ctypes.c_int, w.DWORD]
        self.gdi32.BitBlt.restype = w.BOOL


def _last_error(api_name: str) -> RuntimeError:
    code = ctypes.get_last_error()
    detail = ctypes.FormatError(code).strip() if code else "未提供系统错误码"
    return RuntimeError(f"{api_name} 失败，错误码 {code}: {detail}")


def _is_ghub_path(path: str) -> bool:
    normalized = ntpath.normpath(path).casefold()
    name = ntpath.basename(normalized)
    # G HUB 的界面、托盘和后台进程均使用 lghub*.exe 命名，并安装在 LGHUB 目录中。
    if not re.fullmatch(r"lghub(?:_[a-z0-9_]+)?\.exe", name):
        return False
    return "lghub" in {part for part in normalized.split(ntpath.sep) if part}


def _process_path(api: WinApi, pid: int) -> str | None:
    handle = api.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        capacity = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(capacity.value)
        if not api.kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(capacity)):
            return None
        return buffer.value
    finally:
        api.kernel32.CloseHandle(handle)


def _window_title(api: WinApi, hwnd: int) -> str:
    length = api.user32.GetWindowTextLengthW(hwnd)
    buffer = ctypes.create_unicode_buffer(max(length + 1, 2))
    api.user32.GetWindowTextW(hwnd, buffer, len(buffer))
    return buffer.value


def enumerate_ghub_windows(api: WinApi) -> list[WindowInfo]:
    windows: list[WindowInfo] = []
    paths_by_pid: dict[int, str | None] = {}
    callback_type = api.enum_windows_callback_type

    @callback_type
    def visit(hwnd: int, _lparam: int) -> bool:
        pid = wintypes.DWORD()
        api.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value not in paths_by_pid:
            paths_by_pid[pid.value] = _process_path(api, pid.value)
        process_path = paths_by_pid[pid.value]
        if process_path and _is_ghub_path(process_path):
            windows.append(
                WindowInfo(
                    hwnd=int(hwnd),
                    pid=int(pid.value),
                    process_path=process_path,
                    title=_window_title(api, hwnd),
                    visible=bool(api.user32.IsWindowVisible(hwnd)),
                )
            )
        return True

    if not api.user32.EnumWindows(callback_type(visit), 0):
        raise _last_error("EnumWindows")
    return windows


def _parse_hwnd(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("HWND 请使用十进制或 0x 开头的十六进制数。") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("HWND 必须大于 0。")
    return parsed


def _describe(window: WindowInfo) -> str:
    state = "可见" if window.visible else "隐藏"
    title = window.title if window.title else "<无标题>"
    return f"HWND=0x{window.hwnd:X} PID={window.pid} 进程={window.process_name} 状态={state} 标题={title!r}"


def select_window(args: argparse.Namespace, windows: list[WindowInfo]) -> WindowInfo:
    if args.hwnd is not None:
        matches = [window for window in windows if window.hwnd == args.hwnd]
        if not matches:
            if not ctypes.windll.user32.IsWindow(args.hwnd):
                raise RuntimeError(f"HWND 0x{args.hwnd:X} 无效。")
            raise RuntimeError(f"HWND 0x{args.hwnd:X} 不属于已识别的 Logitech G HUB 进程，拒绝截图。")
    elif args.title is not None:
        matches = [window for window in windows if window.title.casefold() == args.title.casefold()]
    else:
        requested_name = ntpath.basename(args.process).casefold()
        matches = [window for window in windows if window.process_name.casefold() == requested_name]

    if not matches:
        selector = f"标题 {args.title!r}" if args.title is not None else (f"进程 {args.process!r}" if args.process is not None else f"HWND 0x{args.hwnd:X}")
        raise RuntimeError(f"没有找到匹配的 G HUB 窗口（{selector}）。请先运行 --list 查看可用窗口。")
    if len(matches) != 1:
        choices = "\n".join(f"  {_describe(window)}" for window in matches)
        raise RuntimeError(f"选择条件匹配到 {len(matches)} 个 G HUB 窗口；请改用唯一标题或 --hwnd：\n{choices}")
    return matches[0]


def _virtual_screen(api: WinApi) -> tuple[int, int, int, int]:
    return (
        api.user32.GetSystemMetrics(SM_XVIRTUALSCREEN),
        api.user32.GetSystemMetrics(SM_YVIRTUALSCREEN),
        api.user32.GetSystemMetrics(SM_CXVIRTUALSCREEN),
        api.user32.GetSystemMetrics(SM_CYVIRTUALSCREEN),
    )


def _clamp_into_virtual_screen(
    api: WinApi, left: int, top: int, width: int, height: int
) -> tuple[int, int, int, int]:
    """把窗口矩形夹到可见桌面内，保证标题栏区域落在屏幕上。"""
    screen_left, screen_top, screen_width, screen_height = _virtual_screen(api)
    if screen_width <= 0 or screen_height <= 0:
        return left, top, width, height
    width = max(MIN_RESTORED_WIDTH, min(width, screen_width))
    height = max(MIN_RESTORED_HEIGHT, min(height, screen_height))
    left = max(screen_left, min(left, screen_left + screen_width - width))
    top = max(screen_top, min(top, screen_top + screen_height - height))
    # 夹取后再留一点边距，避免窗口贴死屏幕边缘导致桌面回退只截到边框。
    left = max(screen_left, left - 0) + (EDGE_MARGIN if left > screen_left else 0)
    top = max(screen_top, top) + (EDGE_MARGIN if top > screen_top else 0)
    left = min(left, screen_left + screen_width - width)
    top = min(top, screen_top + screen_height - height)
    return left, top, width, height


def restore_window_to_normal(api: WinApi, hwnd: int) -> bool:
    """把最小化或跑到屏幕外的窗口还原成**正常尺寸**并显示（绝不最大化）。

    还原顺序：

    1. ``SetWindowPlacement``：显式写回 ``showCmd=SW_SHOWNORMAL`` 与夹到屏幕内的
       ``rcNormalPosition``。这是唯一能同时「离开最小化」和「保持正常尺寸」的接口，
       也是本函数不使用 ``SW_MAXIMIZE`` 的保证。
    2. 若窗口仍最小化，退到 ``ShowWindowAsync(SW_SHOWNOACTIVATE)``；
    3. 再用 ``SetWindowPos(SWP_SHOWWINDOW)`` 把矩形摆到屏幕内。

    实测（记事本窗口）：只调用 ``SetWindowPos`` 无法让窗口离开最小化状态，必须由
    1 或 2 负责状态切换。G HUB 曾对 ``ShowWindow``/``SetWindowPos(TOPMOST)`` 返回
    Win32 error 5，因此每一步都检查结果并在失败时明确报告。

    契约：已经可见且未最小化的窗口**不做任何改动**（不会去最大化，也不会强行取消
    用户已有的最大化）；只有在必须还原时才写回 ``SW_SHOWNORMAL`` 与正常矩形。
    """
    if not api.user32.IsIconic(hwnd) and api.user32.IsWindowVisible(hwnd):
        # 没最小化也要确认没有整个跑到屏幕外。
        rect = RECT()
        if api.user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            screen_left, screen_top, screen_width, screen_height = _virtual_screen(api)
            visible = (rect.right > screen_left and rect.left < screen_left + screen_width and
                       rect.bottom > screen_top and rect.top < screen_top + screen_height)
            if visible:
                return True

    placement = WINDOWPLACEMENT()
    placement.length = ctypes.sizeof(WINDOWPLACEMENT)
    if not api.user32.GetWindowPlacement(hwnd, ctypes.byref(placement)):
        print(f"警告：{_last_error('GetWindowPlacement')}；无法读取还原位置。", file=sys.stderr)
        return False
    was_maximized = placement.showCmd == SW_SHOWMAXIMIZED
    normal = placement.rcNormalPosition
    width = normal.right - normal.left
    height = normal.bottom - normal.top
    if width <= 0 or height <= 0:
        width, height = MIN_RESTORED_WIDTH, MIN_RESTORED_HEIGHT
    left, top, width, height = _clamp_into_virtual_screen(
        api, normal.left, normal.top, width, height)

    method = "SetWindowPlacement"
    restore_placement = WINDOWPLACEMENT()
    ctypes.memmove(ctypes.byref(restore_placement), ctypes.byref(placement),
                   ctypes.sizeof(WINDOWPLACEMENT))
    restore_placement.showCmd = SW_SHOWNORMAL
    restore_placement.rcNormalPosition = RECT(left, top, left + width, top + height)
    placed = bool(api.user32.SetWindowPlacement(hwnd, ctypes.byref(restore_placement)))
    if not placed:
        print(f"警告：{_last_error('SetWindowPlacement')}；改用 ShowWindowAsync + SetWindowPos。",
              file=sys.stderr)

    if api.user32.IsIconic(hwnd):
        api.user32.ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE)
        method = "ShowWindowAsync" if placed else "ShowWindowAsync(兜底)"
    if not api.user32.IsIconic(hwnd) or not placed:
        moved = bool(api.user32.SetWindowPos(
            hwnd, wintypes.HWND(HWND_TOP_VALUE), left, top, width, height,
            SWP_SHOWWINDOW | SWP_NOACTIVATE,
        ))
        if not moved:
            print(f"警告：{_last_error('SetWindowPos(还原)')}。", file=sys.stderr)
        elif not placed:
            method = "SetWindowPos"

    if was_maximized:
        print("提示：窗口此前处于最大化；已按还原尺寸放回屏幕内（脚本不会最大化窗口）。",
              flush=True)
    iconic = bool(api.user32.IsIconic(hwnd))
    rect = RECT()
    if api.user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        print(f"[还原] 尺寸 {rect.right - rect.left}x{rect.bottom - rect.top} "
              f"@({rect.left},{rect.top})，仍最小化={iconic}，方式={method}", flush=True)
    return not iconic


def _window_dimensions(api: WinApi, window: WindowInfo) -> tuple[RECT, int, int]:
    rect = RECT()
    if not api.user32.GetWindowRect(window.hwnd, ctypes.byref(rect)):
        raise _last_error("GetWindowRect")
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    if width <= 0 or height <= 0:
        raise RuntimeError(f"窗口尺寸无效：{width}x{height}。")
    if width > MAX_BITMAP_DIMENSION or height > MAX_BITMAP_DIMENSION or width * height > MAX_BITMAP_PIXELS:
        raise RuntimeError(f"窗口尺寸过大，拒绝分配截图缓冲区：{width}x{height}。")
    return rect, width, height


class DibSurface:
    def __init__(self, api: WinApi, reference_dc: int, width: int, height: int) -> None:
        self.api = api
        self.width = width
        self.height = height
        self.dc = api.gdi32.CreateCompatibleDC(reference_dc)
        if not self.dc:
            raise _last_error("CreateCompatibleDC")

        self.bitmap_info = BITMAPINFO()
        self.bitmap_info.bmiHeader = BITMAPINFOHEADER(
            ctypes.sizeof(BITMAPINFOHEADER),
            width,
            -height,
            1,
            32,
            BI_RGB,
            width * height * 4,
            0,
            0,
            0,
            0,
        )
        self.pixels = ctypes.c_void_p()
        self.bitmap = api.gdi32.CreateDIBSection(
            reference_dc,
            ctypes.byref(self.bitmap_info),
            DIB_RGB_COLORS,
            ctypes.byref(self.pixels),
            None,
            0,
        )
        if not self.bitmap or not self.pixels:
            api.gdi32.DeleteDC(self.dc)
            raise _last_error("CreateDIBSection")
        self.previous_bitmap = api.gdi32.SelectObject(self.dc, self.bitmap)
        if not self.previous_bitmap:
            api.gdi32.DeleteObject(self.bitmap)
            api.gdi32.DeleteDC(self.dc)
            raise _last_error("SelectObject")

    def to_image(self) -> Image.Image:
        raw = ctypes.string_at(self.pixels, self.width * self.height * 4)
        return Image.frombytes("RGB", (self.width, self.height), raw, "raw", "BGRX")

    def close(self) -> None:
        if self.dc:
            self.api.gdi32.SelectObject(self.dc, self.previous_bitmap)
            self.api.gdi32.DeleteDC(self.dc)
            self.dc = 0
        if self.bitmap:
            self.api.gdi32.DeleteObject(self.bitmap)
            self.bitmap = 0

    def __enter__(self) -> "DibSurface":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def _looks_blank(image: Image.Image) -> bool:
    stats = ImageStat.Stat(image.convert("RGB"))
    return max(stats.stddev, default=0.0) < 0.75


def capture_print_window(api: WinApi, window: WindowInfo, width: int, height: int) -> tuple[Image.Image, bool]:
    reference_dc = api.user32.GetDC(window.hwnd)
    if not reference_dc:
        raise _last_error("GetDC(window)")
    try:
        with DibSurface(api, reference_dc, width, height) as surface:
            success = bool(api.user32.PrintWindow(window.hwnd, surface.dc, PW_RENDERFULLCONTENT))
            return surface.to_image(), success
    finally:
        api.user32.ReleaseDC(window.hwnd, reference_dc)


def capture_visible_desktop(api: WinApi, rect: RECT, width: int, height: int) -> Image.Image:
    screen_x = api.user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
    screen_y = api.user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
    screen_width = api.user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    screen_height = api.user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    left = max(rect.left, screen_x)
    top = max(rect.top, screen_y)
    right = min(rect.right, screen_x + screen_width)
    bottom = min(rect.bottom, screen_y + screen_height)
    if right <= left or bottom <= top:
        raise RuntimeError("G HUB 窗口完全位于桌面可见范围之外，无法使用桌面回退截图。")

    screen_dc = api.user32.GetDC(None)
    if not screen_dc:
        raise _last_error("GetDC(desktop)")
    try:
        with DibSurface(api, screen_dc, width, height) as surface:
            target_x = left - rect.left
            target_y = top - rect.top
            copy_width = right - left
            copy_height = bottom - top
            if not api.gdi32.BitBlt(
                surface.dc,
                target_x,
                target_y,
                copy_width,
                copy_height,
                screen_dc,
                left,
                top,
                SRCCOPY,
            ):
                raise _last_error("BitBlt(desktop visible area)")
            return surface.to_image()
    finally:
        api.user32.ReleaseDC(None, screen_dc)


def capture(args: argparse.Namespace, api: WinApi, window: WindowInfo) -> None:
    if args.restore:
        if not restore_window_to_normal(api, window.hwnd):
            print("警告：窗口仍处于最小化；PrintWindow 很可能返回空白，"
                  "桌面回退也拿不到画面。请手动还原该窗口后重试。", file=sys.stderr)
    elif args.show:
        api.user32.ShowWindow(window.hwnd, SW_RESTORE)
    pinned = bool(api.user32.SetWindowPos(
        window.hwnd,
        wintypes.HWND(HWND_TOPMOST_VALUE),
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
    ))
    if not pinned:
        if not args.allow_unpinned:
            raise _last_error("SetWindowPos(HWND_TOPMOST)")
        print(f"警告：{_last_error('SetWindowPos(HWND_TOPMOST)')}；继续截图，但未确认置顶。", file=sys.stderr)

    if args.foreground and not api.user32.SetForegroundWindow(window.hwnd):
        print("警告：SetForegroundWindow 被 Windows 拒绝；继续尝试截图。", file=sys.stderr)
    if args.foreground or args.show or args.restore:
        time.sleep(0.2)

    rect, width, height = _window_dimensions(api, window)
    image, print_succeeded = capture_print_window(api, window, width, height)
    source = "PrintWindow(PW_RENDERFULLCONTENT)"
    if (not print_succeeded or _looks_blank(image)) and args.fallback_desktop:
        image.close()
        image = capture_visible_desktop(api, rect, width, height)
        source = "桌面 BitBlt 可见区域回退"
    elif not print_succeeded:
        image.close()
        raise RuntimeError("PrintWindow 失败；如需从屏幕可见区域回退，请指定 --fallback-desktop。")
    elif _looks_blank(image):
        image.close()
        raise RuntimeError("PrintWindow 返回空白图像；如需从屏幕可见区域回退，请指定 --fallback-desktop。")

    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        image.save(output, format="PNG")
    finally:
        image.close()
    print(f"{'已始终置顶' if pinned else '未能始终置顶'}，已截图：{_describe(window)}")
    print(f"截图来源：{source}；尺寸：{width}x{height}；文件：{output}")
    if source.startswith("桌面"):
        print("注意：桌面回退只反映屏幕上该矩形的当前可见像素；遮挡部分可能来自其他窗口，屏幕外区域为黑色。")
    else:
        print("注意：PrintWindow 的结果取决于 G HUB 对离屏绘制的支持；硬件加速或受保护内容可能缺失。")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="置顶并截图一个 Logitech G HUB 顶层窗口。",
        epilog=(
            "截图覆盖限制：优先使用 PrintWindow(PW_RENDERFULLCONTENT)，它可能无法呈现硬件加速、遮挡或受保护内容。"
            "指定 --fallback-desktop 后，失败或空白时从桌面 BitBlt 可见区域；被其他窗口遮挡的部分会反映遮挡窗口像素，"
            "完全位于屏幕外的区域无法恢复。脚本只接受安装路径含 LGHUB 且进程名为 lghub*.exe 的窗口。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    selector = parser.add_mutually_exclusive_group()
    selector.add_argument("--title", help="按完整窗口标题精确匹配（忽略大小写）。")
    selector.add_argument("--process", help="按可执行文件名精确匹配，例如 lghub.exe；需唯一窗口。")
    selector.add_argument("--hwnd", type=_parse_hwnd, help="指定窗口句柄，支持十进制或 0x 十六进制。")
    selector.add_argument("--list", action="store_true", help="列出已识别的 G HUB 顶层窗口，不截图。")
    parser.add_argument("--output", help="输出 PNG 文件路径。")
    parser.add_argument("--show", action="store_true", help="恢复并显示窗口后截图。")
    parser.add_argument(
        "--restore",
        action="store_true",
        help="最小化或跑到屏幕外时，按“正常尺寸”还原到屏幕内并显示（不会最大化），再截图；"
             "即使 ShowWindow 被系统拒绝也会用 SetWindowPos 摆回屏幕内。",
    )
    parser.add_argument("--foreground", action="store_true", help="请求将窗口设为前台窗口后截图。")
    parser.add_argument("--allow-unpinned", action="store_true", help="置顶被系统拒绝时仍尝试截图，并明确报告未确认置顶。")
    parser.add_argument("--fallback-desktop", action="store_true", help="PrintWindow 失败/空白时，从桌面可见区域 BitBlt 回退。")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.list:
        if args.output:
            parser.error("--list 不能与 --output 同时使用。")
    else:
        if args.title is None and args.process is None and args.hwnd is None:
            parser.error("截图时必须指定其中一个选择器：--title、--process 或 --hwnd；列举请用 --list。")
        if not args.output:
            parser.error("截图时必须指定 --output。")
        if args.process and not re.fullmatch(r"lghub(?:_[a-z0-9_]+)?\.exe", ntpath.basename(args.process).casefold()):
            parser.error("--process 仅接受 lghub*.exe 进程名。")

    try:
        api = WinApi()
        windows = enumerate_ghub_windows(api)
        if args.list:
            if not windows:
                print("未发现 Logitech G HUB 顶层窗口。")
                return 1
            for window in windows:
                print(_describe(window))
            return 0
        window = select_window(args, windows)
        capture(args, api, window)
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
