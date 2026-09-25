"""直连探针：从电脑侧打开克隆（VID_046D&PID_C092）的厂商 HID 接口，
发一条 HID++ 查询并读回应，用来判定"克隆的厂商通道是死是活"。

为什么需要它：G HUB 显示「连接 Logitech 装备」时，Windows 设备树里克隆的 9 个
节点可能全是 OK——设备在，但厂商通道不响应。这个探针不依赖板子的调试串口，
直接从主机侧发出与 G HUB 同类的请求，看有没有回应。

用法：
    python tools/probe_clone_vendor_hid.py            # 被动监听 2 秒 + 主动查询
    python tools/probe_clone_vendor_hid.py --listen 5 # 只被动监听 5 秒

判定：
    收到 0x11/0x10 开头的回报 → 厂商通道活着（问题在 G HUB 侧）
    只收到空/超时            → 厂商通道不响应（问题在固件侧的厂商上报/响应路径）
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import threading
import time
from ctypes import wintypes

GUID_DEVINTERFACE_HID = "{4D1E55B2-F16F-11CF-88CB-001111000030}"
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x01
FILE_SHARE_WRITE = 0x02
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]
class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", ctypes.POINTER(ctypes.c_ulong))]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("DevicePath", ctypes.c_wchar * 1)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", ctypes.c_ulong), ("VendorID", ctypes.c_ushort),
                ("ProductID", ctypes.c_ushort), ("VersionNumber", ctypes.c_ushort)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", ctypes.c_ushort), ("UsagePage", ctypes.c_ushort),
                ("InputReportByteLength", ctypes.c_ushort),
                ("OutputReportByteLength", ctypes.c_ushort),
                ("FeatureReportByteLength", ctypes.c_ushort),
                ("Reserved", ctypes.c_ushort * 17),
                ("NumberLinkCollectionNodes", ctypes.c_ushort),
                ("NumberInputButtonCaps", ctypes.c_ushort),
                ("NumberInputValueCaps", ctypes.c_ushort),
                ("NumberInputDataIndices", ctypes.c_ushort),
                ("NumberOutputButtonCaps", ctypes.c_ushort),
                ("NumberOutputValueCaps", ctypes.c_ushort),
                ("NumberOutputDataIndices", ctypes.c_ushort),
                ("NumberFeatureButtonCaps", ctypes.c_ushort),
                ("NumberFeatureValueCaps", ctypes.c_ushort),
                ("NumberFeatureDataIndices", ctypes.c_ushort)]


def guid_from_string(text: str) -> GUID:
    value = GUID()
    if ctypes.windll.ole32.CLSIDFromString(ctypes.c_wchar_p(text),
                                           ctypes.byref(value)) != 0:
        raise OSError("GUID 解析失败")
    return value


# 显式声明原型：64 位下句柄必须按指针宽度取回，否则会被截断成 0 个结果。
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), ctypes.c_wchar_p,
                                          ctypes.c_void_p, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD,
    ctypes.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]

kernel32.CreateFileW.restype = ctypes.c_void_p
kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.c_void_p]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CancelIo.restype = wintypes.BOOL
kernel32.CancelIo.argtypes = [ctypes.c_void_p]

hid.HidD_GetAttributes.restype = wintypes.BOOL
hid.HidD_GetAttributes.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
hid.HidD_GetPreparsedData.restype = wintypes.BOOL
hid.HidD_GetPreparsedData.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
hid.HidD_FreePreparsedData.restype = wintypes.BOOL
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.restype = ctypes.c_long
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.c_void_p]


def enumerate_hid_paths() -> list[str]:
    paths: list[str] = []
    guid = guid_from_string(GUID_DEVINTERFACE_HID)
    handle = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if handle == INVALID_HANDLE_VALUE:
        return paths
    try:
        index = 0
        while True:
            interface = SP_DEVICE_INTERFACE_DATA()
            interface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(handle, None, ctypes.byref(guid),
                                                        index, ctypes.byref(interface)):
                break
            index += 1
            needed = wintypes.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                handle, ctypes.byref(interface), None, 0, ctypes.byref(needed), None)
            if needed.value == 0:
                continue
            buffer = ctypes.create_string_buffer(needed.value)
            detail = ctypes.cast(buffer, ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            detail.contents.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)
            if setupapi.SetupDiGetDeviceInterfaceDetailW(
                    handle, ctypes.byref(interface), detail, needed.value,
                    ctypes.byref(needed), None):
                # DevicePath 是变长宽字符串：按 cbSize（=4 字节对齐后的偏移）读整串，
                # 不能通过 c_wchar*1 字段取（那样只会拿到 1 个字符）。
                offset = ctypes.sizeof(wintypes.DWORD)
                paths.append(ctypes.wstring_at(ctypes.addressof(buffer) + offset))
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(handle)
    return paths


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", ctypes.c_void_p)]


kernel32.CreateEventW.restype = ctypes.c_void_p
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL,
                                  ctypes.c_wchar_p]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
kernel32.GetOverlappedResult.restype = wintypes.BOOL
kernel32.GetOverlappedResult.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
ERROR_IO_PENDING = 997
FILE_FLAG_OVERLAPPED = 0x40000000


def overlapped_write(handle, payload: bytes, timeout_ms: int) -> tuple[str, int]:
    """带超时的重叠写：返回 (结果, 字节数)。结果 ∈ {ok, timeout, error}。"""
    event = kernel32.CreateEventW(None, True, False, None)
    overlapped = OVERLAPPED()
    overlapped.hEvent = event
    buffer = ctypes.create_string_buffer(payload, len(payload))
    written = wintypes.DWORD(0)
    ok = kernel32.WriteFile(handle, buffer, len(payload), ctypes.byref(written),
                            ctypes.byref(overlapped))
    if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
        kernel32.CloseHandle(event)
        return "error", 0
    if not ok:
        wait = kernel32.WaitForSingleObject(event, timeout_ms)
        if wait != 0:  # WAIT_OBJECT_0 == 0
            kernel32.CancelIo(handle)
            # 关键：取消后必须等待 I/O 真正结束（bWait=True）再释放事件与结构，
            # 否则内核会写入已释放内存 → 访问冲突崩溃。
            kernel32.GetOverlappedResult(handle, ctypes.byref(overlapped),
                                         ctypes.byref(written), True)
            kernel32.CloseHandle(event)
            return "timeout", 0
        ok = kernel32.GetOverlappedResult(handle, ctypes.byref(overlapped),
                                          ctypes.byref(written), False)
        if not ok:
            kernel32.CloseHandle(event)
            return "error", 0
    kernel32.CloseHandle(event)
    return "ok", written.value


def overlapped_read(handle, size: int, timeout_ms: int) -> tuple[str, bytes]:
    event = kernel32.CreateEventW(None, True, False, None)
    overlapped = OVERLAPPED()
    overlapped.hEvent = event
    buffer = ctypes.create_string_buffer(max(size, 64))
    read = wintypes.DWORD(0)
    ok = kernel32.ReadFile(handle, buffer, len(buffer), ctypes.byref(read),
                           ctypes.byref(overlapped))
    if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
        kernel32.CloseHandle(event)
        return "error", b""
    if not ok:
        if kernel32.WaitForSingleObject(event, timeout_ms) != 0:
            kernel32.CancelIo(handle)
            kernel32.GetOverlappedResult(handle, ctypes.byref(overlapped),
                                         ctypes.byref(read), True)
            kernel32.CloseHandle(event)
            return "timeout", b""
        if not kernel32.GetOverlappedResult(handle, ctypes.byref(overlapped),
                                            ctypes.byref(read), False):
            kernel32.CloseHandle(event)
            return "error", b""
    kernel32.CloseHandle(event)
    return "ok", bytes(buffer.raw[: read.value])


def open_and_describe(path: str):
    handle = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
    if handle == INVALID_HANDLE_VALUE:
        return None, None
    attributes = HIDD_ATTRIBUTES()
    attributes.Size = ctypes.sizeof(HIDD_ATTRIBUTES)
    hid.HidD_GetAttributes(handle, ctypes.byref(attributes))
    preparsed = ctypes.c_void_p()
    caps = None
    if hid.HidD_GetPreparsedData(handle, ctypes.byref(preparsed)):
        caps = HIDP_CAPS()
        hid.HidP_GetCaps(preparsed, ctypes.byref(caps))
        hid.HidD_FreePreparsedData(preparsed)
    return handle, (attributes, caps)


def main() -> int:
    parser = argparse.ArgumentParser(description="厂商 HID 通道直连探针")
    parser.add_argument("--listen", type=float, default=2.0, help="被动监听秒数")
    parser.add_argument("--reply", type=float, default=3.0, help="主动查询后等待回应秒数")
    parser.add_argument("--no-query", action="store_true", help="只监听，不发查询")
    parser.add_argument("--raw", default=None,
                        help="发送指定十六进制 HID++ 负载（如 11ff001000807000）"
                             "并打印回应；用于投递探针实验")
    parser.add_argument("--repeat", type=int, default=1,
                        help="把 --raw 负载重复发送 N 次并统计主机侧往返耗时"
                             "（用于拥堵-vs-拒答的 A/B 判定）")
    parser.add_argument("--hard-timeout", type=float, default=25.0,
                        help="整体硬超时（秒），防止在 HID 读写上永久阻塞")
    args = parser.parse_args()

    # 硬看门狗：HID 的 WriteFile/ReadFile 可能永久阻塞，绝不允许多少时间都耗在里面。
    def watchdog() -> None:
        print(f"（硬超时 {args.hard_timeout:.0f}s 到，强制退出——说明有 HID 读写阻塞）",
              flush=True)
        sys.stdout.flush()
        import os
        os._exit(3)

    timer = threading.Timer(args.hard_timeout, watchdog)
    timer.daemon = True
    timer.start()

    targets = [p for p in enumerate_hid_paths() if "vid_046d&pid_c092" in p.lower()]
    if not targets:
        print("未找到 VID_046D&PID_C092 的 HID 接口（克隆不存在？）")
        return 2
    print(f"找到 {len(targets)} 个克隆 HID 接口")

    vendor_seen = False
    for path in targets:
        handle, info = open_and_describe(path)
        if handle is None:
            print(f"  打开失败：{path}")
            continue
        attributes, caps = info
        if caps is None:
            print(f"  无 caps：{path}")
            kernel32.CloseHandle(handle)
            continue
        kind = "厂商" if caps.UsagePage >= 0xFF00 else "标准"
        print(f"  [{kind}] usage_page=0x{caps.UsagePage:04X} usage=0x{caps.Usage:04X} "
              f"in={caps.InputReportByteLength} out={caps.OutputReportByteLength} "
              f"feature={caps.FeatureReportByteLength}")
        if caps.InputReportByteLength == 0 and caps.OutputReportByteLength == 0:
            kernel32.CloseHandle(handle)
            continue
        vendor_seen = vendor_seen or caps.UsagePage >= 0xFF00

        responses: list[bytes] = []
        stop = threading.Event()

        def reader() -> None:
            size = max(caps.InputReportByteLength, 32)
            while not stop.is_set():
                status, data = overlapped_read(handle, size, 200)
                if status == "ok" and data:
                    responses.append(data)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        time.sleep(args.listen)
        print(f"    被动监听 {args.listen:.1f}s：收到 {len(responses)} 条输入报文"
              + (f"，首条={responses[0][:8].hex(' ')}" if responses else ""))

        if not args.no_query and caps.OutputReportByteLength > 0:
            if args.raw:
                # 投递探针：只发指定负载，打印原样回应（不做 HID++ 语义解析）
                try:
                    payload_bytes = bytes.fromhex(args.raw)
                except ValueError:
                    print(f"    --raw 解析失败：{args.raw}")
                    kernel32.CloseHandle(handle)
                    continue
                if kind != "厂商":
                    kernel32.CloseHandle(handle)
                    continue
                if len(payload_bytes) > caps.OutputReportByteLength:
                    print(f"    负载 {len(payload_bytes)} 字节超过 out="
                          f"{caps.OutputReportByteLength}")
                    kernel32.CloseHandle(handle)
                    continue
                frame = payload_bytes + bytes(
                    caps.OutputReportByteLength - len(payload_bytes))
                if args.repeat > 1:
                    # 重复发送并统计主机侧往返耗时：区分拥堵（耗时整体右移）
                    # 与设备拒答（多数仍很快、个别整笔无回应）。
                    samples: list[float] = []
                    no_reply = 0
                    for _ in range(args.repeat):
                        before = len(responses)
                        started = time.monotonic()
                        status, count = overlapped_write(handle, frame, 1500)
                        if status != "ok":
                            no_reply += 1
                            continue
                        deadline = time.monotonic() + 1.0
                        while time.monotonic() < deadline and len(responses) == before:
                            time.sleep(0.002)
                        if len(responses) == before:
                            no_reply += 1
                        else:
                            samples.append((time.monotonic() - started) * 1000.0)
                        time.sleep(0.005)
                    samples.sort()
                    if samples:
                        def pick(q: float) -> float:
                            index = min(len(samples) - 1,
                                        max(0, int(round(q * (len(samples) - 1)))))
                            return samples[index]
                        print(f"    重复 {args.repeat} 次：有回应 {len(samples)}、"
                              f"无回应 {no_reply}")
                        print(f"      往返耗时 ms：min={samples[0]:.2f} "
                              f"p50={pick(0.5):.2f} p90={pick(0.9):.2f} "
                              f"max={samples[-1]:.2f}")
                    else:
                        print(f"    重复 {args.repeat} 次：全部无回应")
                    stop.set()
                    thread.join(timeout=0.5)
                    kernel32.CloseHandle(handle)
                    continue
                before = len(responses)
                status, count = overlapped_write(handle, frame, 1500)
                print(f"    发送 {payload_bytes.hex(' ')}：{status}({count}B)")
                deadline = time.monotonic() + args.reply
                while time.monotonic() < deadline and len(responses) == before:
                    time.sleep(0.05)
                fresh = responses[before:]
                print(f"      {args.reply:.1f}s 内新收到 {len(fresh)} 条"
                      + (f"，首条={fresh[0][:20].hex(' ')}" if fresh else "（无回应）"))
                stop.set()
                thread.join(timeout=0.5)
                kernel32.CloseHandle(handle)
                continue
            # HID++ 2.0 root.getFeature(0x0000)：0x11 长报文；另试 0x10 短报文。
            long_query = bytes([0x11, 0xFF, 0x00, 0x00, 0x00]) + bytes(15)
            short_query = bytes([0x10, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00])
            for query in (short_query, long_query):
                if len(query) > caps.OutputReportByteLength:
                    continue
                payload = query + bytes(caps.OutputReportByteLength - len(query))
                before = len(responses)
                status, count = overlapped_write(handle, payload, 1500)
                if status == "timeout":
                    print(f"    发送 {query[:3].hex(' ')}…：**写入超时 1500 ms**"
                          f"（设备端不收这条 OUT 报文）")
                    continue
                if status == "error":
                    print(f"    发送 {query[:3].hex(' ')}…：错误码 {ctypes.get_last_error()}")
                    continue
                print(f"    发送 {query[:3].hex(' ')}…：已写出 {count} 字节，等待回应…")
                deadline = time.monotonic() + args.reply
                while time.monotonic() < deadline and len(responses) == before:
                    time.sleep(0.05)
                fresh = responses[before:]
                print(f"      {args.reply:.1f}s 内新收到 {len(fresh)} 条"
                      + (f"，首条={fresh[0][:12].hex(' ')}" if fresh else "（无回应）"))

        stop.set()
        kernel32.CancelIo(handle)
        kernel32.CloseHandle(handle)

    print("结论：", end="")
    if vendor_seen:
        print("已找到厂商接口；若上面显示无回应，说明克隆的厂商通道不响应（固件侧问题）")
    else:
        print("未发现厂商接口（usage_page >= 0xFF00）——克隆可能没暴露厂商集合")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
