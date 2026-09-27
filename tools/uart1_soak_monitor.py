#!/usr/bin/env python3
"""长时串口 soak 监控：持续采集两板 UART0 并统计关键指标，跑完直接看结论。

用途：捕捉间歇性现象（例如"鼠标突然不能移动"），避免事后在几十 MB 原始日志里找。
脚本边收边解析，只把**统计行**抽成时间序列，把**异常事件**抽成时间线；原始日志同时
落盘备查。每 `--snapshot-seconds` 秒写一次进度快照，所以长跑途中也能直接看。

采集的板载统计（固件周期性打印，两板各一份）：
  * `Host HID统计：reports=… vendor_reports=… control=… errors=…`（M 侧，5 秒一次）
  * `HID统计：rate … complete=… vendor_dropped=… motion_skipped=…`（P 侧，1 秒一次）
  * `UART1统计 … rx_ovf=… overflow=… drop=… vendor_drop=… motion_drop=…`
  * `QUEUE name=… received=… rejected=… dropped=… peak=…`（逐队列）
  * `U1EV queue/type/…`（UART1 驱动事件队列）

自动判定要点：
  * **鼠标报告停滞**：M 侧 `reports` 连续 ≥ `--idle-threshold` 秒不增长即记一段停滞，
    并附带该段内 `vendor_reports`/`control` 是否仍在增长 —— 用于区分
    "鼠标休眠/没在用"（vendor 仍活）与"USB Host 或设备卡死"（一起停）。
  * 队列 `rejected`/`dropped`、`FIFO_OVF`、`rx_ovf`、`reset_dropped` 的增量。
  * 克隆挂载/卸载、清理屏障、对端超时、恢复动作等事件的时间点。

用法：
    python tools/uart1_soak_monitor.py --ports COM3,COM13 --labels P,M --seconds 3600
    # 中途看进度：读 outdir 下的 summary-<stamp>-<tag>.partial.json
"""

import argparse
import datetime
import json
import os
import re
import sys
import threading
import time

import serial

# Windows 控制台默认 GBK：统一改成 UTF-8，避免特殊符号（箭头等）打印时崩溃。
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except (AttributeError, ValueError):
    pass

BAUD_DEFAULT = 921600
LINE_RE = re.compile(r'^\[\+\s*([0-9.]+)ms\s+[0-9:.]+\]\s?(.*)$')
HOST_HID_RE = re.compile(
    r'Host HID统计：reports=(\d+) vendor_reports=(\d+) input_fail=(\d+) '
    r'control=(\d+) control_fail=(\d+).*?urb_to=(\d+) urb_retry=(\d+) '
    r'recover=(\d+) port_cycle=(\d+).*?errors=(\d+)')
PC_HID_RE = re.compile(
    r'HID统计：rate .*?failed=(\d+) complete=(\d+) transfer_fail=(\d+).*?'
    r'vendor_rx=(\d+) vendor_submitted=(\d+) vendor_dropped=(\d+).*?'
    r'motion_skipped=(\d+).*?motion_q_peak=(\d+) motion_lat_peak_us=(-?\d+)')
UART_STATS_RE = re.compile(
    r'UART1统计 tx=(\d+) rx=(\d+).*?frame_err=(\d+).*?rx_ovf=(\d+).*?'
    r'rx_pend_peak=(\d+).*?overflow=(\d+) drop=(\d+) write_fail=(\d+) '
    r'vendor_overflow=(\d+) vendor_drop=(\d+)'
    r'(?: motion_overflow=(\d+) motion_drop=(\d+))?')
QUEUE_RE = re.compile(
    r'QUEUE name=(\S+) received=(\d+) rejected=(\d+) dropped=(\d+) peak=(\d+)')
U1EV_QUEUE_RE = re.compile(
    r'U1EV queue current_est=(\d+) waiting=(\d+) peak=(\d+) consumed=(\d+) '
    r'overflow=(\d+) reset_dropped=(\d+)')
U1EV_TYPE_RE = re.compile(r'U1EV type=(\S+) n=(\d+)')
# 帧级收发统计（固件 2026-09-27 起打印）：两板同一行格式，用于跨板流入/流出对照。
FRAME_STATS_RE = re.compile(
    r'UART1帧统计 tx_move=(\d+) rx_move=(\d+) tx_ctrl=(\d+) rx_ctrl=(\d+) '
    r'tx_ack=(\d+) rx_ack=(\d+) tx_beat=(\d+) rx_beat=(\d+) '
    r'tx_prof=(\d+) rx_prof=(\d+) tx_soft=(\d+) rx_soft=(\d+) '
    r'seq_gap=(\d+) seq_evt=(\d+) seq_max=(\d+) seq_back=(\d+)')
FRAME_KEYS = ('tx_move', 'rx_move', 'tx_ctrl', 'rx_ctrl', 'tx_ack', 'rx_ack',
              'tx_beat', 'rx_beat', 'tx_prof', 'rx_prof', 'tx_soft', 'rx_soft',
              'seq_gap', 'seq_evt', 'seq_max', 'seq_back')

# 事件关键词 → 归类名（命中即记录时间点）
EVENT_PATTERNS = (
    ('FIFO接收溢出', 'UART1接收缓冲溢出'),
    ('对端静默超时', 'UART1对端超时'),
    ('克隆已挂载', 'Profile已配置并挂载'),
    ('克隆USB启用', '动态USB严格克隆已启用'),
    ('P侧清理屏障', '开始P侧清理屏障'),
    ('P侧USB清理失败', 'P侧USB清理失败'),
    ('角色锁定', '角色锁定'),
    ('鼠标接口已启动', '动态鼠标输入已启动'),
    ('鼠标枚举发现', 'HID接口发现'),
    ('设备离线事件', 'DEVICE_GONE'),
    # 自愈动作（2026-09-27 起固件打印）：判断"自愈是否生效"的直接证据。
    # 注意必须匹配**实际日志文本**——旧模式 'request_hid_device_recovery' / 'port_cycle'
    # 从未以那种形式出现在日志里，等于一直没命中过。
    ('一级恢复重挂', '自动恢复第'),
    ('二级恢复断电', '二级恢复第'),
    ('数据冻结判据', '设备数据冻结'),
    ('URB孤儿', '孤儿'),
    ('接收缓冲溢出清理', 'FIFO_OVF'),
)
MAX_EVENT_KEEP = 4000


class BoardState:
    def __init__(self, label):
        self.label = label
        self.lock = threading.Lock()
        self.lines = 0
        self.bytes = 0
        self.events = []            # (相对秒, 归类, 原文摘要)
        self.samples = []           # dict：时间序列采样点
        self.queues = {}            # name -> 最新 {received,rejected,dropped,peak}
        self.latest = {}            # 各类统计最新值
        self.idle_spans = []        # 鼠标报告停滞区间
        self._last_reports = None
        self._last_reports_change = None
        self._idle_start = None
        self._idle_vendor = None
        self._idle_control = None
        self.last_line_at = None


def note_event(state, text, at_s):
    """事件匹配必须在所有统计行解析**之后**再做：统计行里含字段名
    （如 `port_cycle=0`、`U1EV type=FIFO_OVF n=0`），先匹配会把它们误记成事件。"""
    for name, token in EVENT_PATTERNS:
        if token in text:
            if len(state.events) < MAX_EVENT_KEEP:
                state.events.append((round(at_s, 2), name, text[:160]))
            return


def parse_into(state, text, at_s, idle_threshold):
    """解析一行日志，更新统计/事件/停滞区间。"""
    match = HOST_HID_RE.search(text)
    if match:
        keys = ('reports', 'vendor_reports', 'input_fail', 'control',
                'control_fail', 'urb_to', 'urb_retry', 'recover',
                'port_cycle', 'errors')
        values = {k: int(v) for k, v in zip(keys, match.groups())}
        with state.lock:
            state.latest['host_hid'] = values
            reports = values['reports']
            if state._last_reports is None:
                state._last_reports = reports
                state._last_reports_change = at_s
            elif reports != state._last_reports:
                if state._idle_start is not None:
                    state.idle_spans.append({
                        'start_s': round(state._idle_start, 2),
                        'end_s': round(at_s, 2),
                        'seconds': round(at_s - state._idle_start, 2),
                        'vendor_delta': values['vendor_reports'] - (state._idle_vendor or 0),
                        'control_delta': values['control'] - (state._idle_control or 0),
                    })
                    state._idle_start = None
                state._last_reports = reports
                state._last_reports_change = at_s
            elif (state._idle_start is None and
                  at_s - state._last_reports_change >= idle_threshold):
                # 记停滞：起点取"最后一次有报告变化"的时刻
                state._idle_start = state._last_reports_change
                state._idle_vendor = values['vendor_reports']
                state._idle_control = values['control']
            state.samples.append({
                't': round(at_s, 2), 'board': state.label, 'kind': 'host_hid',
                'reports': reports, 'vendor_reports': values['vendor_reports'],
                'control': values['control'], 'errors': values['errors'],
            })
        return

    match = PC_HID_RE.search(text)
    if match:
        keys = ('failed', 'complete', 'transfer_fail', 'vendor_rx',
                'vendor_submitted', 'vendor_dropped', 'motion_skipped',
                'motion_q_peak', 'motion_lat_peak_us')
        values = {k: int(v) for k, v in zip(keys, match.groups())}
        with state.lock:
            state.latest['pc_hid'] = values
            state.samples.append({
                't': round(at_s, 2), 'board': state.label, 'kind': 'pc_hid',
                'complete': values['complete'], 'failed': values['failed'],
                'vendor_dropped': values['vendor_dropped'],
                'motion_skipped': values['motion_skipped'],
                # 下面两个字段必须与 build_report 里 pc_hid_delta 的 fields 保持一致：
                # delta_by_key 会跳过任何缺字段的采样，2026-09-27 曾因漏写 motion_q_peak
                # 导致 USB 提交端整段统计静默丢失。
                'motion_q_peak': values['motion_q_peak'],
                'motion_lat_peak_us': values['motion_lat_peak_us'],
            })
        return

    match = UART_STATS_RE.search(text)
    if match:
        keys = ('tx', 'rx', 'frame_err', 'rx_ovf', 'rx_pend_peak', 'overflow',
                'drop', 'write_fail', 'vendor_overflow', 'vendor_drop',
                'motion_overflow', 'motion_drop')
        groups = match.groups()
        values = {}
        for key, raw in zip(keys, groups):
            values[key] = None if raw is None else int(raw)
        with state.lock:
            state.latest['uart'] = values
            state.samples.append({
                't': round(at_s, 2), 'board': state.label, 'kind': 'uart',
                'rx_ovf': values['rx_ovf'], 'overflow': values['overflow'],
                'drop': values['drop'], 'vendor_drop': values['vendor_drop'],
                'motion_drop': values['motion_drop'],
                'rx_pend_peak': values['rx_pend_peak'],
            })
        return

    match = QUEUE_RE.search(text)
    if match:
        name = match.group(1)
        entry = {'received': int(match.group(2)), 'rejected': int(match.group(3)),
                 'dropped': int(match.group(4)), 'peak': int(match.group(5))}
        with state.lock:
            state.queues[name] = entry
            state.samples.append({
                't': round(at_s, 2), 'board': state.label, 'kind': 'queue',
                'queue': name, 'received': entry['received'],
                'rejected': entry['rejected'], 'dropped': entry['dropped'],
                'peak': entry['peak'],
            })
        return

    match = U1EV_QUEUE_RE.search(text)
    if match:
        keys = ('current_est', 'waiting', 'peak', 'consumed', 'overflow',
                'reset_dropped')
        values = {k: int(v) for k, v in zip(keys, match.groups())}
        with state.lock:
            state.latest['u1ev_queue'] = values
            state.samples.append({
                't': round(at_s, 2), 'board': state.label, 'kind': 'u1ev',
                **values,
            })
        return

    match = U1EV_TYPE_RE.search(text)
    if match:
        with state.lock:
            state.latest.setdefault('u1ev_types', {})[match.group(1)] = int(match.group(2))
        return

    match = FRAME_STATS_RE.search(text)
    if match:
        values = {k: int(v) for k, v in zip(FRAME_KEYS, match.groups())}
        with state.lock:
            state.latest['frame_stats'] = values
            state.samples.append({'t': round(at_s, 2), 'board': state.label,
                                  'kind': 'frame', **values})
        return

    note_event(state, text, at_s)


def reader(state, port_name, baud, raw_path, start, stop_event, idle_threshold):
    """一个串口一个线程：读行 → 写原始日志 → 解析。断线自动重连。"""
    while not stop_event.is_set():
        port = serial.Serial()
        port.port = port_name
        port.baudrate = baud
        port.timeout = 0.05
        port.dtr = False
        port.rts = False
        sink = open(raw_path, 'ab')
        try:
            port.open()
            port.dtr = False
            port.rts = False
            port.reset_input_buffer()
        except serial.SerialException as error:
            sink.write(('[[打开失败 %s：%s]]\n' % (port_name, error)).encode('utf-8'))
            sink.close()
            time.sleep(2.0)
            continue
        pending = bytearray()
        try:
            while not stop_event.is_set():
                try:
                    chunk = port.read(4096)
                except serial.SerialException as error:
                    sink.write(('[[读取中断：%s]]\n' % error).encode('utf-8'))
                    break
                if not chunk:
                    continue
                with state.lock:
                    state.bytes += len(chunk)
                sink.write(chunk)
                pending.extend(chunk)
                while b'\n' in pending:
                    raw, _, rest = pending.partition(b'\n')
                    pending = bytearray(rest)
                    text = raw.decode('utf-8', errors='replace').rstrip('\r')
                    match = LINE_RE.match(text)
                    at_s = time.monotonic() - start
                    with state.lock:
                        state.lines += 1
                        state.last_line_at = at_s
                    if match:
                        parse_into(state, match.group(2), at_s, idle_threshold)
                    else:
                        parse_into(state, text, at_s, idle_threshold)
        finally:
            try:
                port.close()
            except serial.SerialException:
                pass
            sink.close()
        if not stop_event.is_set():
            time.sleep(1.0)  # 断线后退避重连


def delta_by_key(samples, kind, key_name, fields):
    """首末采样相减。

    固件里的 received/rejected/dropped/reports 等都是**自板上电累计**，
    所以 soak 期间的有效量必须看增量；`peak` 类字段取期间最大值而非差值。
    key_name 非空时按该字段分组（例如逐队列）。
    """
    first, last = {}, {}
    for sample in samples:
        if sample.get('kind') != kind:
            continue
        if any(sample.get(field) is None for field in fields):
            continue
        group = sample.get(key_name) if key_name else '_'
        if group not in first:
            first[group] = sample
        last[group] = sample
    out = {}
    for group in last:
        low, high = first[group], last[group]
        total = {field: high[field] for field in fields}
        delta = {field: high[field] - low[field] for field in fields}
        for peak_field in ('peak', 'rx_pend_peak', 'motion_q_peak', 'seq_max'):
            if peak_field not in fields:
                continue
            peaks = [s[peak_field] for s in samples
                     if s.get('kind') == kind
                     and (not key_name or s.get(key_name) == group)
                     and s.get(peak_field) is not None]
            best = max(peaks) if peaks else None
            total[peak_field] = best
            delta[peak_field] = best
        out[group] = {'total': total, 'delta': delta}
    return out


def build_report(states, labels, ports, seconds, start_wall, raw_paths,
                 idle_threshold):
    report = {
        'started': start_wall,
        'seconds_requested': seconds,
        'idle_threshold_s': idle_threshold,
        'ports': dict(zip(labels, ports)),
        'raw_logs': raw_paths,
        'boards': {},
    }
    for label in labels:
        state = states[label]
        with state.lock:
            queues = {k: dict(v) for k, v in state.queues.items()}
            latest = {k: (dict(v) if isinstance(v, dict) else v)
                      for k, v in state.latest.items()}
            events = list(state.events)
            idle_spans = list(state.idle_spans)
            if state._idle_start is not None:
                idle_spans.append({
                    'start_s': round(state._idle_start, 2),
                    'end_s': None, 'seconds': None,
                    'vendor_delta': None, 'control_delta': None,
                })
            lines, nbytes = state.lines, state.bytes
            samples = list(state.samples)
            duration = state.last_line_at or 0.0
        report['boards'][label] = {
            'lines': lines,
            'bytes': nbytes,
            'observed_seconds': round(duration, 1),
            'queues': queues,
            'queue_delta': delta_by_key(samples, 'queue', 'queue',
                                        ('received', 'rejected', 'dropped', 'peak')),
            'uart_delta': delta_by_key(samples, 'uart', None,
                                       ('rx_ovf', 'overflow', 'drop', 'vendor_drop',
                                        'motion_drop', 'rx_pend_peak')),
            'host_hid_delta': delta_by_key(samples, 'host_hid', None,
                                           ('reports', 'vendor_reports', 'control',
                                            'errors')),
            'pc_hid_delta': delta_by_key(samples, 'pc_hid', None,
                                         ('complete', 'failed', 'vendor_dropped',
                                          'motion_skipped', 'motion_q_peak')),
            'frame_delta': delta_by_key(samples, 'frame', None, FRAME_KEYS),
            'latest': latest,
            'idle_spans': idle_spans,
            'event_counts': {},
            'events_head': events[:60],
            'events_total': len(events),
        }
        counts = {}
        for _t, name, _text in events:
            counts[name] = counts.get(name, 0) + 1
        report['boards'][label]['event_counts'] = counts
    return report


def judge(report):
    """给出结论要点（不做"通过/失败"式的笼统判定，只列事实与指向）。"""
    notes = []
    m = report['boards'].get('M', {})
    p = report['boards'].get('P', {})
    idle = m.get('idle_spans') or []
    if idle:
        alive = [s for s in idle
                 if (s.get('vendor_delta') or 0) > 0 or (s.get('control_delta') or 0) > 0]
        notes.append('M 侧鼠标报告停滞 %d 段；其中 %d 段期间接收器/控制仍活跃 → 指向'
                     '"鼠标休眠或未使用"，而非桥接故障' % (len(idle), len(alive)))
        for span in idle[:5]:
            notes.append('  停滞 %ss（t=%.1f→%s，vendor_delta=%s control_delta=%s）'
                         % (span.get('seconds'), span['start_s'],
                            span.get('end_s'), span.get('vendor_delta'),
                            span.get('control_delta')))
    else:
        notes.append('M 侧未出现 ≥%d 秒的鼠标报告停滞' % report['idle_threshold_s'])

    def queue_delta(board, name, field):
        q = (report['boards'].get(board, {}).get('queues') or {}).get(name)
        return None if not q else q.get(field)

    for board in ('M', 'P'):
        board_queues = report['boards'].get(board, {}).get('queue_delta') or {}
        if not board_queues:
            continue
        notes.append('--- %s 侧全部队列（本次增量 / 期间峰值；相关队列容量均为 128）---'
                     % board)
        for name in sorted(board_queues):
            entry = board_queues[name]
            d = entry['delta']
            flags = []
            if d['rejected']:
                flags.append('拒收 %d 条' % d['rejected'])
            if d['dropped']:
                flags.append('丢弃 %d 条' % d['dropped'])
            if (d['peak'] or 0) >= 96:
                flags.append('峰值接近容量')
            suffix = ('  [!] ' + '、'.join(flags)) if flags else ''
            notes.append('  %-18s +received=%-8d +rejected=%-6d +dropped=%-6d '
                         'peak=%-4s（累计 received=%d）%s'
                         % (name, d['received'], d['rejected'], d['dropped'],
                            d['peak'], entry['total']['received'], suffix))
    for board in ('M', 'P'):
        uart = (report['boards'].get(board, {}).get('uart_delta') or {}).get('_')
        if uart:
            d = uart['delta']
            notes.append('%s UART1 增量：rx_ovf=%s overflow=%s drop=%s vendor_drop=%s '
                         'motion_drop=%s；期间 rx_pend_peak=%s'
                         % (board, d['rx_ovf'], d['overflow'], d['drop'],
                            d['vendor_drop'], d['motion_drop'], d['rx_pend_peak']))
    host = (m.get('host_hid_delta') or {}).get('_')
    if host:
        d = host['delta']
        notes.append('M 侧本次：reports 增量=%d、vendor_reports 增量=%d、'
                     'control 增量=%d、errors 增量=%d'
                     % (d['reports'], d['vendor_reports'], d['control'], d['errors']))
    pc = (p.get('pc_hid_delta') or {}).get('_')
    if pc:
        d = pc['delta']
        notes.append('P 侧本次：USB complete 增量=%d、failed 增量=%d、'
                     'motion_skipped 增量=%d、vendor_dropped 增量=%d'
                     % (d['complete'], d['failed'], d['motion_skipped'],
                        d['vendor_dropped']))

    m_frame = ((m.get('frame_delta') or {}).get('_') or {}).get('delta') or {}
    p_frame = ((p.get('frame_delta') or {}).get('_') or {}).get('delta') or {}
    if m_frame or p_frame:
        notes.append('--- 板间帧流入/流出（本次增量；源侧 tx -> 目标侧 rx）---')
        items = (
            ('M→P move', m_frame, 'tx_move', p_frame, 'rx_move'),
            ('M→P beat', m_frame, 'tx_beat', p_frame, 'rx_beat'),
            ('M→P ack ', m_frame, 'tx_ack', p_frame, 'rx_ack'),
            ('M→P prof', m_frame, 'tx_prof', p_frame, 'rx_prof'),
            ('M→P soft', m_frame, 'tx_soft', p_frame, 'rx_soft'),
            ('P→M ctrl', p_frame, 'tx_ctrl', m_frame, 'rx_ctrl'),
            ('P→M beat', p_frame, 'tx_beat', m_frame, 'rx_beat'),
            ('P→M ack ', p_frame, 'tx_ack', m_frame, 'rx_ack'),
            ('P→M prof', p_frame, 'tx_prof', m_frame, 'rx_prof'),
            ('P→M soft', p_frame, 'tx_soft', m_frame, 'rx_soft'),
        )
        for label, source, source_key, target, target_key in items:
            sent, got = source.get(source_key), target.get(target_key)
            if sent is None or got is None or (sent == 0 and got == 0):
                continue
            notes.append('  %s：发出 %d → 收到 %d（差 %+d）'
                         % (label, sent, got, got - sent))
        for board, frame in (('M', m_frame), ('P', p_frame)):
            if frame:
                notes.append('  %s 侧帧序号：gap=%s（缺口事件 %s 次、单次最大跳跃 %s）'
                             '；重复/回退丢弃=%s'
                             % (board, frame.get('seq_gap'), frame.get('seq_evt'),
                                frame.get('seq_max'), frame.get('seq_back')))
    counts = p.get('event_counts') or {}
    if counts:
        notes.append('P 侧事件计数：%s' % ', '.join(
            '%s×%d' % (k, v) for k, v in sorted(counts.items())))
    counts_m = m.get('event_counts') or {}
    if counts_m:
        notes.append('M 侧事件计数：%s' % ', '.join(
            '%s×%d' % (k, v) for k, v in sorted(counts_m.items())))
    return notes


def main():
    parser = argparse.ArgumentParser(description='长时串口 soak 监控（两板 UART0）')
    parser.add_argument('--ports', required=True, help='如 COM3,COM13')
    parser.add_argument('--labels', default='P,M')
    parser.add_argument('--baud', type=int, default=BAUD_DEFAULT)
    parser.add_argument('--seconds', type=float, default=3600.0, help='采集时长（默认 1 小时）')
    parser.add_argument('--idle-threshold', type=float, default=30.0,
                        help='M 侧报告停滞判定阈值（秒，默认 30）')
    parser.add_argument('--snapshot-seconds', type=float, default=60.0,
                        help='进度快照间隔（秒，默认 60）')
    parser.add_argument('--outdir', default='artifacts/tests')
    parser.add_argument('--tag', default='soak')
    args = parser.parse_args()

    ports = [item.strip() for item in args.ports.split(',') if item.strip()]
    labels = [item.strip() for item in args.labels.split(',') if item.strip()]
    if len(ports) != len(labels):
        parser.error('--ports 与 --labels 数量必须一致')

    os.makedirs(args.outdir, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    start = time.monotonic()
    start_wall = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    stop_event = threading.Event()

    states = {label: BoardState(label) for label in labels}
    raw_paths = {}
    threads = []
    for label, port_name in zip(labels, ports):
        raw_path = os.path.join(args.outdir, 'soak-%s-%s-%s.raw.log' % (
            label, stamp, args.tag))
        raw_paths[label] = raw_path
        thread = threading.Thread(
            target=reader,
            args=(states[label], port_name, args.baud, raw_path, start,
                  stop_event, args.idle_threshold),
            daemon=True)
        threads.append(thread)
        thread.start()

    summary_path = os.path.join(args.outdir, 'summary-%s-%s.json' % (stamp, args.tag))
    partial_path = os.path.join(args.outdir,
                                'summary-%s-%s.partial.json' % (stamp, args.tag))
    print('soak 开始：%s，时长 %.0f 秒；原始日志 → %s' % (
        ', '.join('%s=%s' % (l, p) for l, p in zip(labels, ports)),
        args.seconds, os.path.dirname(list(raw_paths.values())[0])), flush=True)

    next_snapshot = start + args.snapshot_seconds
    try:
        while time.monotonic() - start < args.seconds:
            time.sleep(0.5)
            if time.monotonic() >= next_snapshot:
                next_snapshot = time.monotonic() + args.snapshot_seconds
                report = build_report(states, labels, ports,
                                      time.monotonic() - start, start_wall,
                                      raw_paths, args.idle_threshold)
                report['partial'] = True
                with open(partial_path, 'w', encoding='utf-8') as handle:
                    json.dump(report, handle, ensure_ascii=False, indent=2)
                elapsed = time.monotonic() - start
                m = report['boards'].get('M', {})
                p = report['boards'].get('P', {})
                print('[+%6.0fs] M 行=%d 停滞段=%d | P 行=%d | 快照 %s' % (
                    elapsed, m.get('lines', 0), len(m.get('idle_spans') or []),
                    p.get('lines', 0), os.path.basename(partial_path)), flush=True)
    except KeyboardInterrupt:
        print('收到中断，提前汇总…', flush=True)
    finally:
        stop_event.set()
        for thread in threads:
            thread.join(timeout=5.0)

    report = build_report(states, labels, ports, time.monotonic() - start,
                          start_wall, raw_paths, args.idle_threshold)
    report['notes'] = judge(report)
    with open(summary_path, 'w', encoding='utf-8') as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)

    # 时间序列 CSV（便于画图/二次分析）
    csv_path = os.path.join(args.outdir, 'soak-%s-%s.csv' % (stamp, args.tag))
    with open(csv_path, 'w', encoding='utf-8') as handle:
        handle.write('t,board,kind,key,value\n')
        for label in labels:
            with states[label].lock:
                samples = list(states[label].samples)
            for sample in samples:
                for key, value in sample.items():
                    if key in ('t', 'board', 'kind'):
                        continue
                    handle.write('%s,%s,%s,%s,%s\n' % (
                        sample['t'], sample['board'], sample['kind'], key, value))

    print('\n===== soak 汇总 =====')
    print('时长 %.0f 秒（起 %s）' % (time.monotonic() - start, start_wall))
    for label in labels:
        board = report['boards'][label]
        print('%s：行数=%d 字节=%d 事件=%d；原始日志 %s' % (
            label, board['lines'], board['bytes'], board['events_total'],
            raw_paths[label]))
    print('\n--- 结论要点 ---')
    for note in report['notes']:
        print('  ' + note)
    print('\n摘要 JSON：%s' % summary_path)
    print('时间序列 CSV：%s' % csv_path)
    print('（长跑途中可随时读 %s 看进度）' % partial_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
