#!/usr/bin/env python3
"""运行时切换板载日志写盘（默认不写盘，需要时再打开）。

固件在 `ONBOARD_LOG_PAUSED_AT_BOOT=1` 下开机即暂停写盘（完全不碰 flash，
避免 flash 擦写期间 cache 关、双核停饿到 UART1 接收）。本工具用 UART0 协议
命令 `LOG_CONTROL_REQUEST(0x0C)` 在运行时恢复/暂停写盘，并用同一条命令的
回报读取当前状态与板端已落盘字节数。

用法：
    python tools/set_onboard_log_write.py --port COM3 --status
    python tools/set_onboard_log_write.py --port COM3 --enable --verify-seconds 4
    python tools/set_onboard_log_write.py --port COM3 --disable

语义与限制：
  * 只发协议帧，不动 DTR/RTS（不会复位板子）。
  * 状态只存在 RAM：任何复位/断电后回到编译期默认值（当前为“暂停/不写盘”）。
  * `total` 是板端已落盘字节数，不含尚未落盘的缓冲；暂停期间它保持不变。
  * 两板各有一套板载日志，要对哪块板生效就指定哪一块的 UART0 端口。
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serial  # noqa: E402

from fetch_onboard_log import (  # noqa: E402
    FrameScanner,
    MESSAGE_LOG_CONTROL_REQUEST,
    build_frame,
    read_response,
)

BAUD_DEFAULT = 921600
REPLY_TIMEOUT_S = 3.0


def exchange(port, scanner, sequence, payload):
    """发一条 LOG_CONTROL_REQUEST 并读回状态，返回 (结果, 下一个序号)。"""
    port.write(build_frame(MESSAGE_LOG_CONTROL_REQUEST, sequence, payload))
    port.flush()
    result = read_response(scanner, port, sequence,
                           time.monotonic() + REPLY_TIMEOUT_S)
    return result, (sequence + 1) & 0xFFFF


def describe(result):
    if result is None:
        return None
    _offset, total, data = result
    paused = data[0] if data else None
    return paused, total


def main() -> int:
    parser = argparse.ArgumentParser(
        description='运行时切换/查询板载日志写盘（LOG_CONTROL_REQUEST 0x0C）')
    parser.add_argument('--port', required=True, help='目标板的 UART0 端口，如 COM3')
    parser.add_argument('--baud', type=int, default=BAUD_DEFAULT)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--enable', action='store_true', help='恢复板载写盘')
    group.add_argument('--disable', action='store_true', help='暂停板载写盘')
    group.add_argument('--status', action='store_true', help='只查询当前状态')
    parser.add_argument('--verify-seconds', type=float, default=0.0,
                        help='切换后等待 N 秒再查一次，用 total 增长判断写盘是否真的在推进')
    args = parser.parse_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.05
    # 显式放开 DTR/RTS：查询/切换不应该复位被测板。
    port.dtr = False
    port.rts = False
    try:
        port.open()
        port.dtr = False
        port.rts = False
    except serial.SerialException as error:
        print('打开 %s 失败：%s' % (args.port, error), file=sys.stderr)
        return 2
    port.reset_input_buffer()

    scanner = FrameScanner()
    sequence = 1
    try:
        if args.enable:
            payload, action = b'\x00', '恢复板载写盘'
        elif args.disable:
            payload, action = b'\x01', '暂停板载写盘'
        else:
            payload, action = b'', '查询状态'

        result, sequence = exchange(port, scanner, sequence, payload)
        state = describe(result)
        if state is None:
            print('[%s] 无响应（%s）：板子是否在线、端口是否为该板 UART0？'
                  % (action, args.port), file=sys.stderr)
            return 1
        paused, total = state
        print('[%s] 板端回报：%s total=%d 字节' % (
            action,
            'paused=1（不写盘）' if paused else 'paused=0（写盘中）',
            total))

        if args.verify_seconds > 0:
            time.sleep(args.verify_seconds)
            probe, sequence = exchange(port, scanner, sequence, b'')
            probe_state = describe(probe)
            if probe_state is None:
                print('[校验] 第二次查询无响应', file=sys.stderr)
                return 1
            paused2, total2 = probe_state
            delta = total2 - total
            print('[校验] %.1f 秒后：%s total=%d（增量 %+d 字节）' % (
                args.verify_seconds,
                'paused=1（不写盘）' if paused2 else 'paused=0（写盘中）',
                total2, delta))
            if paused2:
                print('[校验] 结论：停止写盘，total 变化仅来自切换前的残留缓冲')
            elif delta > 0:
                print('[校验] 结论：PASS —— 写盘已恢复并在推进')
            else:
                print('[校验] 结论：写盘已恢复，但这段时间没有新日志落盘'
                      '（空闲或日志量不足以触发落盘）')
    finally:
        port.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
