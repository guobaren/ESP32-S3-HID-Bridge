"""HID++ 2.0 直通探针（主机端，经克隆的厂商集合）。

用途：投递探针实验——发送"会改变设备状态"的 HID++ 请求（如改灯光颜色/DPI），
用来判断**一笔控制传输到底有没有送达设备**：
  * 状态确实变了 → 消息到达了设备（设备收到却不回）；
  * 状态没变     → 消息很可能根本没发出去（问题在我们的 USB 主机栈）。

不需要改固件：克隆在电脑侧暴露了厂商集合（usage_page 0xFF00 / usage 0x0002，
20 字节），写入即由 P 板转发给 M 板，再作为控制传输发给物理鼠标。

用法示例：
    python tools/hidpp_probe.py --query 0x0000 0x8070      # root.getFeature(0x8070)
    python tools/hidpp_probe.py --raw 11ff001000807000...  # 直接发原始负载
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)

kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.CreateEventW.argtypes = [
    ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.ReadFile.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.GetOverlappedResult.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD),
    wintypes.BOOL]
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD), ("Reserved", ctypes.c_void_p),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


# setupapi / hid 的返回值必须显式声明原型，否则 64 位句柄会被截断成 int（曾因此枚举不到设备）
setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [
    ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID),
    wintypes.DWORD, ctypes.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
hid.HidP_GetCaps.restype = ctypes.c_int
hid.HidP_GetCaps.argtypes = [wintypes.HANDLE, ctypes.c_void_p]


def find_vendor_paths() -> list[str]:
    """列出所有 usage_page=0xFF00 且 usage=0x0002 的 HID 集合路径。"""
    hid_guid = GUID(0x4D1E55B2, 0xF16F, 0x11CF,
                    (ctypes.c_ubyte * 8)(0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30))
    info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(hid_guid), None, None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    paths: list[str] = []
    index = 0
    while True:
        data = SP_DEVICE_INTERFACE_DATA()
        data.cbSize = ctypes.sizeof(data)
        if not setupapi.SetupDiEnumDeviceInterfaces(
                info, None, ctypes.byref(hid_guid), index, ctypes.byref(data)):
            break
        index += 1
        needed = wintypes.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(
            info, ctypes.byref(data), None, 0, ctypes.byref(needed), None)
        buffer = ctypes.create_string_buffer(needed.value)
        ctypes.cast(buffer, ctypes.POINTER(wintypes.DWORD))[0] = (
            ctypes.sizeof(wintypes.DWORD) * 2 + ctypes.sizeof(wintypes.WCHAR))
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                info, ctypes.byref(data), buffer, needed.value,
                ctypes.byref(needed), None):
            continue
        path = ctypes.wstring_at(buffer, ctypes.sizeof(wintypes.DWORD) * 2)
        handle = kernel32.CreateFileW(
            path, 0, 0x1 | 0x2, None, OPEN_EXISTING, 0, None)
        if handle == INVALID_HANDLE_VALUE:
            continue
        caps = HIDP_CAPS()
        if hid.HidP_GetCaps(handle, ctypes.byref(caps)) and \
                caps.UsagePage == 0xFF00 and caps.Usage == 0x0002:
            paths.append(path)
        kernel32.CloseHandle(handle)
    setupapi.SetupDiDestroyDeviceInfoList(info)
    return paths


class VendorChannel:
    def __init__(self, path: str) -> None:
        self.handle = kernel32.CreateFileW(
            path, GENERIC_READ | GENERIC_WRITE, 0x1 | 0x2, None,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if self.handle == INVALID_HANDLE_VALUE:
            raise OSError(f"打开失败：{ctypes.get_last_error()}")
        caps = HIDP_CAPS()
        hid.HidP_GetCaps(self.handle, ctypes.byref(caps))
        self.out_len = caps.OutputReportByteLength
        self.in_len = caps.InputReportByteLength

    def close(self) -> None:
        if self.handle and self.handle != INVALID_HANDLE_VALUE:
            kernel32.CancelIoEx(self.handle, None)
            kernel32.CloseHandle(self.handle)
            self.handle = None

    def _overlapped(self) -> tuple[OVERLAPPED, int]:
        event = kernel32.CreateEventW(None, True, False, None)
        overlapped = OVERLAPPED()
        overlapped.hEvent = event
        return overlapped, event

    def write(self, payload: bytes, timeout_ms: int = 2000) -> bool:
        buffer = ctypes.create_string_buffer(payload, len(payload))
        overlapped, event = self._overlapped()
        written = wintypes.DWORD(0)
        ok = kernel32.WriteFile(
            self.handle, buffer, len(payload), ctypes.byref(written),
            ctypes.byref(overlapped))
        if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
            kernel32.CloseHandle(event)
            return False
        wait = kernel32.WaitForSingleObject(event, timeout_ms)
        if wait != WAIT_OBJECT_0:
            kernel32.CancelIoEx(self.handle, ctypes.byref(overlapped))
            kernel32.WaitForSingleObject(event, 500)
            kernel32.CloseHandle(event)
            return False
        kernel32.GetOverlappedResult(
            self.handle, ctypes.byref(overlapped), ctypes.byref(written), False)
        kernel32.CloseHandle(event)
        return True

    def read(self, timeout_ms: int = 2000) -> bytes | None:
        buffer = ctypes.create_string_buffer(self.in_len)
        overlapped, event = self._overlapped()
        count = wintypes.DWORD(0)
        ok = kernel32.ReadFile(
            self.handle, buffer, self.in_len, ctypes.byref(count),
            ctypes.byref(overlapped))
        if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
            kernel32.CloseHandle(event)
            return None
        wait = kernel32.WaitForSingleObject(event, timeout_ms)
        if wait != WAIT_OBJECT_0:
            kernel32.CancelIoEx(self.handle, ctypes.byref(overlapped))
            kernel32.WaitForSingleObject(event, 500)
            kernel32.CloseHandle(event)
            return None
        kernel32.GetOverlappedResult(
            self.handle, ctypes.byref(overlapped), ctypes.byref(count), False)
        kernel32.CloseHandle(event)
        data = buffer.raw[:count.value]
        return data

    def request(self, payload: bytes, wait_ms: int = 1500) -> bytes | None:
        """发送一条请求并等待匹配 swId 的回应。"""
        if len(payload) < 4:
            raise ValueError("负载太短")
        sw_id = payload[3] & 0x0F
        frame = payload + bytes(max(0, self.out_len - len(payload)))
        if not self.write(frame[:self.out_len]):
            return None
        deadline = time.monotonic() + wait_ms / 1000.0
        while time.monotonic() < deadline:
            data = self.read(timeout_ms=200)
            if not data:
                continue
            # 回报：0x11 <dev> <feature> <function<<4|swId> ...
            if len(data) >= 4 and data[0] == 0x11 and (data[3] & 0x0F) == sw_id:
                return data
        return None


def get_feature(channel: VendorChannel, feature_id: int, sw_id: int = 0) -> int | None:
    """root.getFeature(featureId) → 返回 featureIndex（0 表示不支持）。"""
    payload = bytes([0x11, 0xFF, 0x00, 0x10 | sw_id,
                     (feature_id >> 8) & 0xFF, feature_id & 0xFF, 0x00])
    response = channel.request(payload)
    if response is None:
        return None
    return response[4]


def main() -> int:
    parser = argparse.ArgumentParser(description="HID++ 2.0 直通探针")
    parser.add_argument("--query", nargs="+", default=None,
                        help="查询功能索引：--query 0x8070 0x2201 ...")
    parser.add_argument("--raw", default=None,
                        help="直接发送十六进制负载（如 11ff001000807000）")
    parser.add_argument("--wait", type=float, default=1.5, help="等待回应秒数")
    args = parser.parse_args()

    paths = find_vendor_paths()
    if not paths:
        print("未找到厂商集合（克隆是否已挂载？）", file=sys.stderr)
        return 2
    channel = VendorChannel(paths[0])
    print(f"厂商集合：in={channel.in_len} out={channel.out_len}")
    try:
        if args.raw:
            payload = bytes.fromhex(args.raw)
            response = channel.request(payload, int(args.wait * 1000))
            print(f"发送 {payload.hex(' ')}")
            print(f"回应 {response.hex(' ') if response else '（无）'}")
            return 0 if response else 1
        if args.query:
            for item in args.query:
                feature_id = int(item, 0)
                index = get_feature(channel, feature_id)
                if index is None:
                    print(f"  0x{feature_id:04X}: 无回应")
                else:
                    print(f"  0x{feature_id:04X}: featureIndex=0x{index:02X}"
                          f"{'（不支持）' if index == 0 else ''}")
            return 0
        # 默认：探测常见可改状态的功能
        for feature_id in (0x0000, 0x0001, 0x8070, 0x8071, 0x2201, 0x2205,
                           0x8060, 0x8061, 0x8100, 0x1814):
            index = get_feature(channel, feature_id)
            print(f"  0x{feature_id:04X}: "
                  f"{'无回应' if index is None else f'featureIndex=0x{index:02X}'}")
        return 0
    finally:
        channel.close()


if __name__ == "__main__":
    raise SystemExit(main())
