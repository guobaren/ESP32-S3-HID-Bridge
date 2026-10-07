#!/usr/bin/env python3
"""双板 UART0 日志同步采集 + 显式 RTS 复位（只读，不发任何协议帧）。

用途：同时记录鼠标侧（M）与电脑侧（P）的串口日志，并在指定时刻只复位其中一
块板，用于核对恢复时序。原「连接恢复修复方案」阶段文档已删除，恢复时序的现
状与边界以 docs/连接流程.md 与 docs/交接.md 为准。

时间戳：每个端口一条时间线，行首为相对采集开始的毫秒数，便于两板对齐。

后插的板子：串口在采集开始时不存在的会每 0.5s 重试打开，出现时打印相对时间
（例如按 O3/O4 线序复现时鼠标侧板还没上电）。`--reset-port` 指定的板子如果
还没出现，会等它出现后再复位，而不是直接判失败。

`--ports auto`：不写死串口名，自动采集**当前所有串口**以及采集期间新出现的
串口（插拔后 Windows 可能改成 COM14 这类新名字）。标签就是串口名，M/P 由日志
内容区分（`dual_pc_hid` 是电脑侧板 P，`dual_hid_host`/`dual_uart0` 是鼠标侧板 M）。

复位：默认不复位；--reset-port 指定的端口在 --reset-after 秒后执行一次
"硬复位"脉冲（DTR=False 保持正常启动，RTS=True→False 拉低再释放 EN）。

示例：
    python tools/capture_dual_serial.py --ports auto --seconds 300 --tag o3-repro
    python tools/capture_dual_serial.py --ports COM9,COM13 --labels M,P \\
        --seconds 30 --reset-port COM13 --reset-after 8
"""

import argparse
import datetime
import os
import sys
import threading
import time

import serial
from serial.tools import list_ports

BAUD_DEFAULT = 921600


class PortRecorder(threading.Thread):
    def __init__(self, label, port_name, baud, path, start_monotonic, retry_deadline=None):
        super().__init__(daemon=True)
        self.label = label
        self.port_name = port_name
        self.baud = baud
        self.path = path
        self.start_monotonic = start_monotonic
        # 停止重试的绝对时刻：采集窗口结束后留一段宽限，避免进程变成孤儿后
        # 无限重试、长期占住串口（job_kill 只杀包装进程，Python 子进程会活下来）。
        self.retry_deadline = retry_deadline
        self.port = None
        self.stop_requested = threading.Event()
        self.bytes_read = 0
        self.lines = 0
        self.open_error = None
        self.opened = threading.Event()
        self.appeared_at = None

    def _try_open(self):
        try:
            self.port = serial.Serial()
            self.port.port = self.port_name
            self.port.baudrate = self.baud
            self.port.timeout = 0.05
            self.port.write_timeout = 0.5
            # 打开前先放开 DTR/RTS，尽量避免打开本身触发自动复位电路。
            self.port.dtr = False
            self.port.rts = False
            self.port.open()
            self.port.dtr = False
            self.port.rts = False
            self.port.reset_input_buffer()
            return True
        except serial.SerialException as error:
            self.open_error = str(error)
            self.port = None
            return False

    def run(self):
        # 串口可能尚未插入（例如按 O3/O4 线序复现时鼠标侧板还没上电）：
        # 持续重试直到出现、采集结束或超过宽限期，出现时打印相对时间便于和插线动作对齐。
        while not self.stop_requested.is_set():
            if self.retry_deadline is not None and time.monotonic() >= self.retry_deadline:
                break
            if self._try_open():
                break
            time.sleep(0.5)
        if self.port is None:
            self.opened.set()
            return
        self.appeared_at = time.monotonic() - self.start_monotonic
        self.opened.set()
        print("[%s] %s 于 t=+%.2fs 打开并开始记录" % (
            self.label, self.port_name, self.appeared_at), flush=True)
        pending = bytearray()
        with open(self.path, 'wb') as sink:
            while not self.stop_requested.is_set():
                try:
                    chunk = self.port.read(4096)
                except serial.SerialException as error:
                    sink.write(('[[读取中断: %s]]\n' % error).encode('utf-8'))
                    break
                if chunk:
                    self.bytes_read += len(chunk)
                    now = time.monotonic() - self.start_monotonic
                    pending.extend(chunk)
                    while b'\n' in pending:
                        raw, _, rest = pending.partition(b'\n')
                        pending = bytearray(rest)
                        self._write_line(sink, now, raw)
        with open(self.path, 'ab') as sink:
            if pending:
                self._write_line(sink, time.monotonic() - self.start_monotonic,
                                 bytes(pending))
        self.close()

    def _write_line(self, sink, now, raw):
        text = raw.decode('utf-8', errors='replace').rstrip('\r')
        stamp = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        sink.write(('[+%9.3fms %s] %s\n' % (now * 1000.0, stamp, text)).encode('utf-8'))
        sink.flush()
        self.lines += 1

    def hard_reset(self):
        """硬复位：DTR 保持 False（正常启动），RTS 拉低再释放 EN。"""
        if self.port is None or not self.port.is_open:
            return False
        self.port.dtr = False
        time.sleep(0.05)
        self.port.rts = True
        time.sleep(0.15)
        self.port.rts = False
        time.sleep(0.05)
        return True

    def close(self):
        if self.port is not None and self.port.is_open:
            try:
                self.port.close()
            except serial.SerialException:
                pass


def main():
    parser = argparse.ArgumentParser(description='双板 UART0 日志采集（只读）')
    parser.add_argument('--ports', required=True,
                        help="逗号分隔的串口列表（例如 COM9,COM13），或 auto 表示"
                             "自动采集当前及采集期间新出现的所有串口")
    parser.add_argument('--labels', default='',
                        help='逗号分隔的标签，缺省用串口名')
    parser.add_argument('--baud', type=int, default=BAUD_DEFAULT)
    parser.add_argument('--seconds', type=float, default=30.0,
                        help='采集时长')
    parser.add_argument('--reset-port', default='',
                        help='在这一路执行一次硬复位（缺省不复位）')
    parser.add_argument('--reset-after', type=float, default=8.0,
                        help='开始采集后多少秒执行复位')
    parser.add_argument('--outdir', default='artifacts/tests')
    parser.add_argument('--tag', default='',
                        help='日志文件名后缀标记')
    args = parser.parse_args()

    ports = [item.strip() for item in args.ports.split(',') if item.strip()]
    auto_ports = len(ports) == 1 and ports[0].lower() == 'auto'
    labels = [item.strip() for item in args.labels.split(',') if item.strip()]
    if len(labels) != len(ports):
        labels = [port.replace('COM', 'P') for port in ports]
    if args.reset_port and not auto_ports and args.reset_port not in ports:
        parser.error('--reset-port 必须是 --ports 中的一项')

    os.makedirs(args.outdir, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    suffix = ('-' + args.tag) if args.tag else ''
    start_monotonic = time.monotonic()

    recorders = []
    known_ports = set()
    retry_deadline = start_monotonic + args.seconds + 60.0

    def add_recorder(label, port):
        path = os.path.join(args.outdir, 'dual-serial-%s-%s%s.log' % (label, stamp, suffix))
        recorder = PortRecorder(label, port, args.baud, path, start_monotonic, retry_deadline)
        recorders.append(recorder)
        known_ports.add(port)
        recorder.start()
        return recorder

    if auto_ports:
        # 不写死串口名：先接当前已有的，之后由监督线程补上插拔后新出现的串口。
        initial = sorted(port.device for port in list_ports.comports())
        for port in initial:
            add_recorder(port, port)
        print('自动模式：当前串口 %s' % (', '.join(initial) if initial else '（无）'))
    else:
        for label, port in zip(labels, ports):
            add_recorder(label, port)

    # 只等很短时间：晚插的板子会在后台继续重试打开，不阻塞采集开始。
    for recorder in recorders:
        recorder.opened.wait(3.0)

    print('采集开始：%s，时长 %.1fs' % (
        ', '.join('%s=%s' % (r.label, r.port_name) for r in recorders) or '（暂无串口）',
        args.seconds))
    for rec in recorders:
        print('  %s -> %s' % (rec.label, rec.path))
    missing = [rec for rec in recorders if rec.port is None]
    for rec in missing:
        print('  %s(%s) 尚未出现，将在后台每 0.5s 重试打开' % (rec.label, rec.port_name))

    reset_done = False
    deadline = start_monotonic + args.seconds
    next_port_scan = start_monotonic
    while time.monotonic() < deadline:
        time.sleep(0.2)
        if auto_ports and time.monotonic() >= next_port_scan:
            next_port_scan = time.monotonic() + 1.0
            for port in sorted(item.device for item in list_ports.comports()):
                if port in known_ports:
                    continue
                print('[+%.2fs] 发现新串口 %s，开始记录' % (
                    time.monotonic() - start_monotonic, port), flush=True)
                add_recorder(port, port)
                recorders[-1].opened.wait(2.0)
        if (args.reset_port and not reset_done and
                time.monotonic() - start_monotonic >= args.reset_after):
            target = next((rec for rec in recorders
                           if rec.port_name == args.reset_port), None)
            if target is not None and target.port is not None and target.hard_reset():
                reset_done = True
                print('已在 t=+%.2fs 对 %s (%s) 执行硬复位' % (
                    time.monotonic() - start_monotonic, target.label,
                    target.port_name))
            elif target is not None and target.port is None:
                # 目标串口还没出现：继续等，避免“复位失败”被误读成硬件问题。
                continue
            elif target is None and auto_ports:
                continue
            else:
                print('复位失败：%s' % args.reset_port, file=sys.stderr)
                reset_done = True

    for recorder in recorders:
        recorder.stop_requested.set()
    for recorder in recorders:
        recorder.join(timeout=5.0)

    print('采集结束：')
    for recorder in recorders:
        if recorder.port is None and recorder.bytes_read == 0:
            print('  %s 始终未出现（port=%s）：%s' % (
                recorder.label, recorder.port_name,
                recorder.open_error or '未提供系统错误'))
            continue
        print('  %s bytes=%d lines=%d 出现于 t=+%.2fs -> %s' % (
            recorder.label, recorder.bytes_read, recorder.lines,
            recorder.appeared_at or 0.0, recorder.path))
    return 0


if __name__ == '__main__':
    sys.exit(main())
