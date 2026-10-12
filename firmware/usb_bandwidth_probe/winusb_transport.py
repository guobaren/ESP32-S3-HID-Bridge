"""专用 USB 探针接口的 WinUSB 传输；不更换或安装设备驱动。"""
import ctypes as C
from ctypes import wintypes as W
from collections import deque
import os
import struct
import uuid

GUID_TEXT = '76AB5DF1-AB4B-45E5-9C3C-9041D7808491'
# 极端取消超时情况下保持对象存活，禁止释放仍被内核使用的缓冲。
_UNFINISHED = []


class GUID(C.Structure):
    _fields_ = [('bytes', C.c_ubyte * 16)]


class Interface(C.Structure):
    _fields_ = [('cbSize', W.DWORD), ('guid', GUID), ('flags', W.DWORD), ('reserved', C.c_size_t)]


class Overlapped(C.Structure):
    _fields_ = [('internal', C.c_size_t), ('internal_high', C.c_size_t),
                ('offset', W.DWORD), ('offset_high', W.DWORD), ('event', W.HANDLE)]


class Pipe(C.Structure):
    _fields_ = [('type', C.c_int), ('id', C.c_ubyte), ('maximum_packet', W.WORD), ('interval', C.c_ubyte)]


class WinUSB:
    def __init__(self, queue_depth=1):
        if os.name != 'nt':
            raise RuntimeError('WinUSB 仅支持 Windows')
        if not 1 <= queue_depth <= 16:
            raise ValueError('队列深度必须为1到16')
        self.depth = queue_depth
        self.pending = deque()
        self.cached = bytearray()
        self.file = self.usb = None
        self.k = C.WinDLL('kernel32', use_last_error=True)
        self.u = C.WinDLL('winusb', use_last_error=True)
        self.s = C.WinDLL('setupapi', use_last_error=True)
        self._bind()

    def _bind(self):
        signatures = [
            (self.k, 'CreateFileW', W.HANDLE, [W.LPCWSTR, W.DWORD, W.DWORD, W.LPVOID, W.DWORD, W.DWORD, W.HANDLE]),
            (self.k, 'CloseHandle', W.BOOL, [W.HANDLE]),
            (self.k, 'CreateEventW', W.HANDLE, [W.LPVOID, W.BOOL, W.BOOL, W.LPCWSTR]),
            (self.k, 'WaitForSingleObject', W.DWORD, [W.HANDLE, W.DWORD]),
            (self.k, 'CancelIoEx', W.BOOL, [W.HANDLE, C.POINTER(Overlapped)]),
            (self.s, 'SetupDiGetClassDevsW', W.HANDLE, [C.POINTER(GUID), W.LPCWSTR, W.HWND, W.DWORD]),
            (self.s, 'SetupDiEnumDeviceInterfaces', W.BOOL, [W.HANDLE, W.LPVOID, C.POINTER(GUID), W.DWORD, C.POINTER(Interface)]),
            (self.s, 'SetupDiGetDeviceInterfaceDetailW', W.BOOL, [W.HANDLE, C.POINTER(Interface), W.LPVOID, W.DWORD, C.POINTER(W.DWORD), W.LPVOID]),
            (self.s, 'SetupDiDestroyDeviceInfoList', W.BOOL, [W.HANDLE]),
            (self.u, 'WinUsb_Initialize', W.BOOL, [W.HANDLE, C.POINTER(W.HANDLE)]),
            (self.u, 'WinUsb_Free', W.BOOL, [W.HANDLE]),
            (self.u, 'WinUsb_QueryPipe', W.BOOL, [W.HANDLE, C.c_ubyte, C.c_ubyte, C.POINTER(Pipe)]),
            (self.u, 'WinUsb_GetDescriptor', W.BOOL, [W.HANDLE, C.c_ubyte, C.c_ubyte, W.WORD, W.LPVOID, W.DWORD, C.POINTER(W.DWORD)]),
            (self.u, 'WinUsb_SetPipePolicy', W.BOOL, [W.HANDLE, C.c_ubyte, W.DWORD, W.DWORD, W.LPVOID]),
            (self.u, 'WinUsb_GetPipePolicy', W.BOOL, [W.HANDLE, C.c_ubyte, W.DWORD, C.POINTER(W.DWORD), W.LPVOID]),
            (self.u, 'WinUsb_GetOverlappedResult', W.BOOL, [W.HANDLE, C.POINTER(Overlapped), C.POINTER(W.DWORD), W.BOOL]),
        ]
        for name in ('WinUsb_ReadPipe', 'WinUsb_WritePipe'):
            signatures.append((self.u, name, W.BOOL, [W.HANDLE, C.c_ubyte, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped)]))
        for dll, name, restype, args in signatures:
            fn = getattr(dll, name)
            fn.restype, fn.argtypes = restype, args

    @staticmethod
    def check(ok, operation):
        if not ok:
            raise OSError(C.get_last_error(), operation)

    def _path(self):
        guid = GUID.from_buffer_copy(uuid.UUID(GUID_TEXT).bytes_le)
        devices = self.s.SetupDiGetClassDevsW(C.byref(guid), None, None, 0x12)
        if devices == C.c_void_p(-1).value:
            raise OSError(C.get_last_error(), '枚举专用WinUSB接口失败')
        paths = []
        try:
            index = 0
            while True:
                interface = Interface(cbSize=C.sizeof(Interface))
                if not self.s.SetupDiEnumDeviceInterfaces(devices, None, C.byref(guid), index, C.byref(interface)):
                    if C.get_last_error() != 259:
                        raise OSError(C.get_last_error(), '枚举WinUSB接口失败')
                    break
                needed = W.DWORD()
                self.s.SetupDiGetDeviceInterfaceDetailW(devices, C.byref(interface), None, 0, C.byref(needed), None)
                if needed.value < 8:
                    raise RuntimeError('WinUSB接口路径长度无效')
                detail = C.create_string_buffer(needed.value)
                C.cast(detail, C.POINTER(W.DWORD))[0] = 8 if C.sizeof(C.c_void_p) == 8 else 6
                self.check(self.s.SetupDiGetDeviceInterfaceDetailW(devices, C.byref(interface), detail, needed, None, None), '读取专用接口路径失败')
                path = C.wstring_at(C.addressof(detail) + 4)
                if 'vid_303a&pid_400a' in path.lower():
                    paths.append(path)
                index += 1
        finally:
            self.s.SetupDiDestroyDeviceInfoList(devices)
        if len(paths) != 1:
            raise RuntimeError(f'需要恰好一个303A:400A WinUSB探针，当前数量{len(paths)}')
        return paths[0]

    def __enter__(self):
        try:
            self.file = self.k.CreateFileW(self._path(), 0xc0000000, 3, None, 3, 0x40000000, None)
            if self.file == C.c_void_p(-1).value:
                self.file = None
                raise OSError(C.get_last_error(), '打开专用WinUSB接口失败')
            usb = W.HANDLE()
            self.check(self.u.WinUsb_Initialize(self.file, C.byref(usb)), '初始化WinUSB失败')
            self.usb = usb.value
            desc, got = C.create_string_buffer(18), W.DWORD()
            self.check(self.u.WinUsb_GetDescriptor(self.usb, 1, 0, 0, desc, 18, C.byref(got)), '读取USB描述符失败')
            if got.value != 18 or struct.unpack_from('<HH', desc.raw, 8) != (0x303a, 0x400a):
                raise RuntimeError('USB设备描述符与测试接口不一致')
            pipes = set()
            for index in range(2):
                pipe = Pipe()
                self.check(self.u.WinUsb_QueryPipe(self.usb, 0, index, C.byref(pipe)), '读取Bulk端点失败')
                if pipe.type != 2 or pipe.maximum_packet != 64:
                    raise RuntimeError('必须为全速Bulk64端点')
                pipes.add(pipe.id)
            if pipes != {0x01, 0x81}:
                raise RuntimeError('Bulk端点地址不一致')
            # 输出请求以ZLP结束；使设备多包接收也能处理不足8KB的最后一批。
            policy = C.c_ubyte(1)
            self.check(self.u.WinUsb_SetPipePolicy(self.usb, 1, 1, 1, C.byref(policy)), '设置SHORT_PACKET_TERMINATE失败')
            timeout = W.DWORD(30000)
            for pipe in (1, 0x81):
                self.check(self.u.WinUsb_SetPipePolicy(self.usb, pipe, 3, 4, C.byref(timeout)), '设置管道超时失败')
            maximum, size = W.DWORD(), W.DWORD(4)
            self.check(self.u.WinUsb_GetPipePolicy(self.usb, 1, 8, C.byref(size), C.byref(maximum)), '读取最大请求长度失败')
            if maximum.value < 64:
                raise RuntimeError('最大Bulk请求长度无效')
            self.maximum_write = maximum.value
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def _submit(self, data=None, length=0):
        buffer = C.create_string_buffer(data, len(data)) if data is not None else C.create_string_buffer(length)
        event = self.k.CreateEventW(None, True, False, None)
        self.check(event, '创建USB完成事件失败')
        ov, got = Overlapped(event=event), W.DWORD()
        item = (buffer, ov, got, len(data) if data is not None else length, data is not None)
        fn = self.u.WinUsb_WritePipe if data is not None else self.u.WinUsb_ReadPipe
        ok = fn(self.usb, 1 if data is not None else 0x81, buffer, item[3], C.byref(got), C.byref(ov))
        if not ok and C.get_last_error() != 997:
            self.k.CloseHandle(event)
            raise OSError(C.get_last_error(), '提交USB请求失败')
        self.pending.append(item)
        return item

    def _finish(self, item):
        buffer, ov, got, wanted, writing = item
        status = self.k.WaitForSingleObject(ov.event, 30000)
        if status != 0:
            raise TimeoutError(f'USB请求等待失败，状态{status}')
        self.check(self.u.WinUsb_GetOverlappedResult(self.usb, C.byref(ov), C.byref(got), False), 'USB请求完成失败')
        if writing and got.value != wanted:
            raise IOError('USB实际完成短写')
        self.pending.remove(item)
        self.k.CloseHandle(ov.event)
        return buffer.raw[:got.value]

    def queue_write(self, data):
        for offset in range(0, len(data), self.maximum_write):
            while len(self.pending) >= self.depth:
                self._finish(self.pending[0])
            self._submit(data[offset:offset + self.maximum_write])
        return len(data)

    def drain(self):
        while self.pending:
            self._finish(self.pending[0])

    def write(self, data):
        self.queue_write(data)
        self.drain()
        return len(data)

    def read(self, length):
        self.drain()
        if not self.cached:
            self.cached.extend(self._finish(self._submit(length=8192)))
        result = bytes(self.cached[:length])
        del self.cached[:len(result)]
        return result

    def __exit__(self, *_):
        if self.file and self.pending:
            self.k.CancelIoEx(self.file, None)
            while self.pending:
                item = self.pending[0]
                # 取消完成前，保持请求、缓冲和事件有效。
                status = self.k.WaitForSingleObject(item[1].event, 30000)
                if status != 0:
                    _UNFINISHED.append(self)
                    raise TimeoutError('USB取消尚未完成，拒绝释放在途缓冲')
                got = W.DWORD()
                self.u.WinUsb_GetOverlappedResult(self.usb, C.byref(item[1]), C.byref(got), False)
                self.pending.popleft()
                self.k.CloseHandle(item[1].event)
        if self.usb:
            self.u.WinUsb_Free(self.usb)
            self.usb = None
        if self.file:
            self.k.CloseHandle(self.file)
            self.file = None
