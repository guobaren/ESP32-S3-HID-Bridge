"""真实原生 USB CDC 校验测速；禁止将 CH340 维护口作为测量对象。"""
import argparse
import json
import struct
import time
import zlib
from pathlib import Path
import serial
from serial.tools import list_ports

MAGIC = 0x31504255
HEADER = struct.Struct('<5I')
BLOCK = 4096
PATTERNS = [bytes((i + s) & 255 for i in range(BLOCK)) for s in range(256)]


def packet(kind, seq=0, data=b''):
    return HEADER.pack(MAGIC, kind, seq, len(data), zlib.crc32(data)) + data


def exact(port, length):
    data = bytearray()
    deadline = time.monotonic() + 30
    while len(data) < length:
        part = port.read(length - len(data))
        if part:
            data.extend(part)
        elif time.monotonic() > deadline:
            raise TimeoutError('USB 接收超时')
    return bytes(data)


def receive(port):
    magic, kind, seq, length, crc = HEADER.unpack(exact(port, HEADER.size))
    if magic != MAGIC or length > BLOCK:
        raise ValueError('非法USB测试帧')
    data = exact(port, length)
    if zlib.crc32(data) != crc:
        raise ValueError('USB测试帧CRC失败')
    return kind, seq, data


def write(port, data):
    if port.write(data) != len(data):
        raise IOError('USB短写')


def statistics(data):
    names = ('bytes', 'elapsed_us', 'crc_errors', 'sequence_errors', 'blocks')
    if len(data) != 40:
        raise ValueError('统计长度错误')
    return dict(zip(names, struct.unpack('<5Q', data)))


def rate(result, elapsed):
    result['host_elapsed_seconds'] = elapsed
    result['payload_MB_per_second'] = result['bytes'] / elapsed / 1e6
    result['payload_Mbps'] = result['bytes'] * 8 / elapsed / 1e6
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--transport', choices=('cdc', 'winusb'), default='cdc')
    parser.add_argument('--port')
    parser.add_argument('--queue-depth', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--batch-blocks', type=int, default=1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 10 <= args.seconds <= 60:
        parser.error('每个方向应持续10到60秒')
    if not 1 <= args.batch_blocks <= 64:
        parser.error('批量块数应为1到64')
    if not 1 <= args.queue_depth <= 16:
        parser.error('队列深度必须为1到16')
    if args.transport == 'cdc':
        match = next((p for p in list_ports.comports() if p.device == args.port), None)
        if match is None or (match.vid, match.pid) != (0x303A, 0x4009):
            raise RuntimeError('必须选择原生USB测试接口303A:4009，不能选择CH340或其他设备')
        connection = serial.Serial(args.port, 115200, timeout=1, write_timeout=30)
        hello_expected = b'USB_BANDWIDTH_PROBE_V1;FS;BULK64'
    else:
        from winusb_transport import WinUSB
        connection = WinUSB(args.queue_depth)
        hello_expected = b'USB_BANDWIDTH_PROBE_V1;FS;VENDOR64'
    result = {'port': args.port, 'transport': 'native USB FS ' + args.transport + ' bulk',
              'queue_depth': args.queue_depth if args.transport == 'winusb' else 1,
              'block_payload_bytes': BLOCK, 'batch_blocks': args.batch_blocks, 'requested_seconds': args.seconds,
              'passed': False}
    try:
        with connection as port:
            # 该测试固件未实现DTR复位；这里开启CDC会话，不是UART切速。
            if args.transport == 'cdc':
                port.dtr = True
                port.rts = False
            write(port, packet(1))
            kind, _, hello = receive(port)
            if kind != 1 or hello != hello_expected:
                raise ValueError('设备握手不匹配')
            write(port, packet(10))
            if receive(port) != (10, 0, b''):
                raise ValueError('发送方向初始化失败')
            seq = 0
            start = time.perf_counter()
            while time.perf_counter() - start < args.seconds:
                batch = b''.join(packet(2, s, PATTERNS[s & 255])
                                 for s in range(seq, seq + args.batch_blocks))
                if args.transport == 'winusb':
                    port.queue_write(batch)
                else:
                    write(port, batch)
                seq += args.batch_blocks
            if args.transport == 'winusb':
                port.drain()
            write(port, packet(3))
            kind, _, data = receive(port)
            elapsed = time.perf_counter() - start
            if kind != 5:
                raise ValueError('缺少固件接收统计')
            down = statistics(data)
            if (down['bytes'], down['blocks'], down['crc_errors'], down['sequence_errors']) != (seq * BLOCK, seq, 0, 0):
                raise ValueError(f'固件实际接收校验失败: {down}')
            result['PC_to_ESP32'] = rate(down, elapsed)
            write(port, packet(4, data=struct.pack('<I', args.seconds * 1000)))
            start = time.perf_counter()
            seq = 0
            while True:
                kind, received_seq, data = receive(port)
                if kind == 5:
                    up = statistics(data)
                    break
                if kind != 6 or received_seq != seq or data != PATTERNS[seq & 255]:
                    raise ValueError('电脑接收序号/数据内容错误')
                seq += 1
            elapsed = time.perf_counter() - start
            if up['bytes'] != seq * BLOCK or up['blocks'] != seq:
                raise ValueError('固件发送计数与电脑校验计数不一致')
            result['ESP32_to_PC'] = rate(up, elapsed)
            result['passed'] = True
    except Exception as exc:
        result['error'] = str(exc)
        raise
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
