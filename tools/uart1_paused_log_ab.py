#!/usr/bin/env python3
"""板载写盘暂停 A/B：清计数 → 静默 10 秒 → 复位 M → 统计 20 秒。

背景（docs/审计.md · Known Issues · uart1_link.c:2105-2133,2269）：
P 侧快速移动样本在 UART1 RX FIFO 满阈值 120 与 96 时都各有 2 次硬件
`FIFO_OVF`，而事件队列与 16 KB 驱动 RX 环形缓冲都不积压（rx_pend_peak
仅 134/214 字节）。据此怀疑瓶颈在 ISR 侧：flash 写/擦除期间 cache 关、
两个核都停，UART ISR（代码在 flash，CONFIG_UART_ISR_IN_IRAM 未开）跑不了，
硬件 128 字节 FIFO 在 1.39 ms 内填满即溢出。

本脚本用固件的 `ONBOARD_LOG_PAUSED_AT_BOOT=1`（开机即不写 flash）做对照：
A/B 条件是编译期的，所以必须先刷入该版本再运行本脚本。

时序（默认值，可用参数覆盖）：
    t=0            开始采集，并依次硬复位两板 → RAM 统计计数清零
    t≈0..10s       静默稳定期（不计入统计）
    t≈10s          响铃提示操作者准备并开始持续快速移动鼠标
    t≈13s          基线窗口（--pre-seconds）
    t≈16s          对 M 硬复位（测试动作）
    t≈16..36s      统计窗口（--post-seconds，默认 20 秒）

只读采集：不发送任何协议帧，只用 DTR/RTS 做硬复位。

已采集数据的复算（判定规则或统计口径改动后使用，不重新占用串口）：
    python tools/uart1_paused_log_ab.py --analyze-only \\
        --summary artifacts/tests/summary-<stamp>-<tag>.json

示例：
    python tools/uart1_paused_log_ab.py --ports COM3,COM13 --labels P,M \\
        --reset-label M --tag paused-log-ab
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_dual_serial import PortRecorder  # noqa: E402

BAUD_DEFAULT = 921600
LINE_RE = re.compile(r'^\[\+\s*([0-9.]+)ms\s+[0-9:.]+\]\s?(.*)$')
STATS_RE = re.compile(r'rx_ovf=(\d+).*?rx_pend_peak=(\d+)')
RXBYTES_RE = re.compile(r'rx_bytes=(\d+)')
QUEUE_RE = re.compile(
    r'U1EV queue current_est=(\d+) waiting=(\d+) peak=(\d+) consumed=(\d+) '
    r'overflow=(\d+) reset_dropped=(\d+)')
TYPE_RE = re.compile(r'U1EV type=(\S+) n=(\d+)')
HANDLE_RE = re.compile(r'U1EV handle n=(\d+) sum_us=(\d+) max_us=(\d+)')
BREAK_RE = re.compile(r'U1EV break seen=(\d+)')
# 固件逐队列输出的 received(到达入口的尝试) / rejected(入队被拒) / dropped(主动丢弃) / peak(峰值)。
QSTATS_RE = re.compile(
    r'QUEUE name=(\S+) received=(\d+) rejected=(\d+) dropped=(\d+) peak=(\d+)')
# UART1 统计行里与丢失相关的累计计数（q/peak 为应用 TX 队列，drop 为整队清理丢弃）。
UART_EXTRA_RE = re.compile(
    r'reject=(\d+) self=(\d+) rx_ovf=(\d+).*?q=(\d+) peak=(\d+) overflow=(\d+) '
    r'drop=(\d+) write_fail=(\d+) vendor_overflow=(\d+) vendor_drop=(\d+)')
# HID 统计行里的丢弃/放弃口径（两板字段不同，取交集与各自特有项）。
COUNTER_NAMES = (
    'vendor_dropped', 'motion_skipped', 'motion_merged', 'transfer_fail',
    'input_fail', 'failed', 'not_ready',
    'control_fail', 'cleanup_retry', 'ack_retry', 'errors',
)
COUNTER_RE = re.compile(r'\b(' + '|'.join(COUNTER_NAMES) + r')=(\d+)')
REPORTS_RE = re.compile(r'reports=(\d+)')
OVERFLOW_WARN = 'UART1接收缓冲溢出'
PAUSED_MARK = '板载写盘开机即暂停'


def parse_log(path):
    """把日志行拆成 (相对毫秒, 正文) 列表。"""
    rows = []
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        for line in handle:
            match = LINE_RE.match(line.rstrip('\n'))
            if match:
                rows.append((float(match.group(1)), match.group(2)))
    return rows


def summarize(rows, begin_ms, end_ms):
    """统计 [begin_ms, end_ms) 窗口内的关键指标（累计量取窗口内最大值）。"""
    info = {
        'lines': 0,
        'overflow_warnings': 0,
        'fifo_ovf_events': 0,       # U1EV type=FIFO_OVF n= 求和（每个窗口块内计数）
        'data_events': 0,
        'break_events': 0,
        'rx_ovf_last': 0,
        'rx_bytes_first': None,
        'rx_bytes_last': 0,
        'rx_pend_peak_max': 0,
        'queue_peak_max': 0,        # U1EV 事件队列估算峰值
        'queue_est_max': 0,
        'reset_dropped_last': 0,
        'consumed_last': 0,
        'handle_max_us': 0,
        'paused_mark': False,
        'overflow_times_ms': [],
        'queues': {},               # 逐队列 received/rejected/dropped/peak
        'uart_extra': {},           # UART1 统计行的丢失相关字段
        'counters': {},             # HID 统计行的丢弃/放弃计数
    }
    for at_ms, text in rows:
        if not (begin_ms <= at_ms < end_ms):
            continue
        info['lines'] += 1
        if PAUSED_MARK in text:
            info['paused_mark'] = True
        if OVERFLOW_WARN in text:
            info['overflow_warnings'] += 1
            info['overflow_times_ms'].append(round(at_ms, 1))
        match = STATS_RE.search(text)
        if match:
            info['rx_ovf_last'] = max(info['rx_ovf_last'], int(match.group(1)))
            info['rx_pend_peak_max'] = max(
                info['rx_pend_peak_max'], int(match.group(2)))
        match = RXBYTES_RE.search(text)
        if match:
            value = int(match.group(1))
            if info['rx_bytes_first'] is None:
                info['rx_bytes_first'] = value
            info['rx_bytes_last'] = value
        match = QUEUE_RE.search(text)
        if match:
            info['queue_est_max'] = max(info['queue_est_max'], int(match.group(1)))
            info['queue_peak_max'] = max(info['queue_peak_max'], int(match.group(3)))
            info['consumed_last'] = max(info['consumed_last'], int(match.group(4)))
            info['reset_dropped_last'] = max(
                info['reset_dropped_last'], int(match.group(6)))
        match = QSTATS_RE.search(text)
        if match:
            name = match.group(1)
            entry = info['queues'].setdefault(
                name, {'received': 0, 'rejected': 0, 'dropped': 0, 'peak': 0})
            for index, key in enumerate(('received', 'rejected', 'dropped', 'peak'),
                                        start=2):
                entry[key] = max(entry[key], int(match.group(index)))
        match = UART_EXTRA_RE.search(text)
        if match:
            keys = ('reject', 'self', 'rx_ovf', 'tx_q', 'tx_q_peak', 'overflow',
                    'drop', 'write_fail', 'vendor_overflow', 'vendor_drop')
            for key, value in zip(keys, match.groups()):
                info['uart_extra'][key] = max(info['uart_extra'].get(key, 0),
                                              int(value))
        match = TYPE_RE.search(text)
        if match:
            kind, count = match.group(1), int(match.group(2))
            if kind == 'FIFO_OVF':
                info['fifo_ovf_events'] += count
            elif kind == 'DATA':
                info['data_events'] += count
            elif kind == 'BREAK':
                info['break_events'] += count
        match = HANDLE_RE.search(text)
        if match:
            info['handle_max_us'] = max(info['handle_max_us'], int(match.group(3)))
        for name, value in COUNTER_RE.findall(text):
            info['counters'][name] = max(info['counters'].get(name, 0), int(value))
    if info['rx_bytes_first'] is not None:
        info['rx_bytes_delta'] = info['rx_bytes_last'] - info['rx_bytes_first']
    else:
        info['rx_bytes_delta'] = None
    return info


def queue_delta(before, after):
    """两个相邻窗口的逐队列差值：窗口内新增的 received/rejected/dropped。"""
    result = {}
    names = set(before.get('queues', {})) | set(after.get('queues', {}))
    for name in sorted(names):
        low = before.get('queues', {}).get(
            name, {'received': 0, 'rejected': 0, 'dropped': 0, 'peak': 0})
        high = after.get('queues', {}).get(
            name, {'received': 0, 'rejected': 0, 'dropped': 0, 'peak': 0})
        result[name] = {
            'received': high['received'] - low['received'],
            'rejected': high['rejected'] - low['rejected'],
            'dropped': high['dropped'] - low['dropped'],
            'peak_abs': high['peak'],
        }
    return result


def counter_delta(before, after):
    result = {}
    names = set(before.get('counters', {})) | set(after.get('counters', {}))
    for name in sorted(names):
        low = before.get('counters', {}).get(name, 0)
        high = after.get('counters', {}).get(name, 0)
        result[name] = {'delta': high - low, 'abs': high}
    return result


def report_extremes(rows, begin_ms, end_ms):
    """M 侧实体报告数的区间：确认统计窗口内确实有输入负载。"""
    low = None
    high = None
    for at_ms, text in rows:
        if not (begin_ms <= at_ms < end_ms):
            continue
        match = REPORTS_RE.search(text)
        if match:
            value = int(match.group(1))
            low = value if low is None else min(low, value)
            high = value if high is None else max(high, value)
    if low is None:
        return None, None
    return low, high


def build_report(tag, stamp, reset_label, reset_at, pre_at, post_seconds,
                 min_reports, log_paths, ports):
    """从日志文件计算前后窗口指标与判定（采集与复算共用）。"""
    report = {
        'tag': tag,
        'stamp': stamp,
        'ports': ports,
        'reset_label': reset_label,
        'reset_at_s': round(reset_at, 3),
        'window_pre_s': [round(pre_at, 3), round(reset_at, 3)],
        'window_post_s': [round(reset_at, 3), round(reset_at + post_seconds, 3)],
        'logs': log_paths,
    }
    post_end = reset_at + post_seconds
    rows_p = parse_log(log_paths['P']) if 'P' in log_paths else []
    if rows_p:
        report['P_pre'] = summarize(rows_p, pre_at * 1000.0, reset_at * 1000.0)
        report['P_post'] = summarize(rows_p, reset_at * 1000.0, post_end * 1000.0)
        # 暂停标记只在启动期打印，必须按整段采集扫描，否则会误判“条件未生效”。
        report['P_full'] = summarize(rows_p, 0.0, float('inf'))
        report['P_queues_full'] = report['P_full']['queues']
        if reset_label == 'P':
            # 被复位的那块板在窗口起点丢了 RAM 计数，post−pre 没有物理意义：
            # 用窗口末值表示"复位后累计"，避免把清零前后相减得出错误增量。
            report['P_queues_after_reset'] = report['P_post']['queues']
        else:
            report['P_queues_post_delta'] = queue_delta(report['P_pre'],
                                                        report['P_post'])
        report['P_counters_full'] = report['P_full']['counters']
        report['P_counters_post_delta'] = counter_delta(report['P_pre'],
                                                        report['P_post'])
    rows_m = parse_log(log_paths['M']) if 'M' in log_paths else []
    if rows_m:
        low, high = report_extremes(rows_m, reset_at * 1000.0, post_end * 1000.0)
        report['M_reports'] = {'first': low, 'last': high}
        report['M_report_delta'] = (None if low is None or high is None
                                    else high - low)
        report['M_pre'] = summarize(rows_m, pre_at * 1000.0, reset_at * 1000.0)
        report['M_post'] = summarize(rows_m, reset_at * 1000.0, post_end * 1000.0)
        report['M_full'] = summarize(rows_m, 0.0, float('inf'))
        report['M_queues_full'] = report['M_full']['queues']
        if reset_label == 'M':
            # 同上：M 在窗口起点被硬复位，改用复位后累计值。
            report['M_queues_after_reset'] = report['M_post']['queues']
        else:
            report['M_queues_post_delta'] = queue_delta(report['M_pre'],
                                                        report['M_post'])
        report['M_counters_full'] = report['M_full']['counters']
        report['M_counters_post_delta'] = counter_delta(report['M_pre'],
                                                        report['M_post'])

    post = report.get('P_post')
    full = report.get('P_full') or {}
    conditions = {
        # 开机暂停标记出现在启动期，因此按整段采集判定条件是否生效。
        'paused_condition_seen': bool(full.get('paused_mark')),
        'paused_mark_in_window': bool(post and post['paused_mark']),
        'load_reports_delta': report.get('M_report_delta'),
        'load_ok': (report.get('M_report_delta') or 0) >= min_reports,
    }
    report['conditions'] = conditions

    if post is None:
        verdict = '无法判定：未取到 P 侧日志'
    elif not conditions['paused_condition_seen']:
        verdict = ('无效：整段采集内都没有“板载写盘开机即暂停”标记，'
                   'A/B 条件未生效（是否刷入 ONBOARD_LOG_PAUSED_AT_BOOT=1 版本？）')
    elif not conditions['load_ok']:
        verdict = ('无效样本：统计窗口内 M 侧实体报告增量 %s 低于下限 %d，'
                   '无法与历史快速移动样本对比' % (
                       conditions['load_reports_delta'], min_reports))
    elif post['overflow_warnings'] == 0 and post['fifo_ovf_events'] == 0:
        verdict = ('支持写盘假设：暂停写盘后统计窗口内 FIFO_OVF = 0'
                   '（历史同类快速移动窗口为 2 次）')
    else:
        verdict = ('不支持单一写盘成因：暂停写盘后统计窗口内仍有 FIFO_OVF %d 次'
                   '（U1EV 计数 %d）' % (post['overflow_warnings'],
                                        post['fifo_ovf_events']))
    report['verdict'] = verdict
    return report


def notify(message):
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          'notify_user.ps1')
    if not os.path.exists(script):
        print('（未找到 notify_user.ps1，跳过提示音）')
        return
    try:
        subprocess.run(['pwsh', '-NoProfile', '-File', script,
                        '-Times', '2', '-Message', message],
                       check=False, timeout=30)
    except Exception as error:  # noqa: BLE001 - 提示音失败不应中断测试
        print('提示音调用失败：%s' % error)


def print_queues(title, full, delta):
    """逐队列打印：峰值（整段累计）与统计窗口内的 received/rejected/dropped。"""
    print('--- %s 队列（peak=整段累计峰值；received/rejected/dropped=统计窗口内增量）---'
          % title)
    if not full and not delta:
        print('    （日志里没有 QUEUE 行）')
        return
    names = sorted(set(full) | set(delta))
    print('    %-20s %10s %10s %10s %10s' % ('队列', 'received', 'rejected',
                                             'dropped', 'peak'))
    for name in names:
        d = delta.get(name, {'received': 0, 'rejected': 0, 'dropped': 0,
                             'peak_abs': 0})
        f = full.get(name, {'peak': d['peak_abs']})
        print('    %-20s %10d %10d %10d %10d' % (
            name, d['received'], d['rejected'], d['dropped'],
            max(f.get('peak', 0), d['peak_abs'])))


def print_counters(title, full, delta):
    if not full:
        return
    parts = []
    for name in sorted(full):
        d = delta.get(name, {}).get('delta', 0)
        parts.append('%s=%d(+%d)' % (name, full[name], d))
    print('--- %s 丢弃/放弃计数（累计(+统计窗口增量)）---' % title)
    print('    ' + ' '.join(parts))


def print_summary(report, post_seconds, min_reports):
    pre = report.get('P_pre')
    post = report.get('P_post')
    print('\n===== 汇总 =====')
    if pre:
        print('复位前基线窗口：溢出警告=%d 事件=%d rx_ovf=%d rx_pend_peak=%d' % (
            pre['overflow_warnings'], pre['fifo_ovf_events'], pre['rx_ovf_last'],
            pre['rx_pend_peak_max']))
    if post:
        rate = None
        if post['rx_bytes_delta'] is not None:
            rate = post['rx_bytes_delta'] / post_seconds
        print('统计窗口(%.0fs)：溢出警告=%d FIFO_OVF事件=%d DATA事件=%d '
              'rx_ovf=%d rx_pend_peak=%d 事件队列peak=%d reset_dropped=%d '
              'BREAK=%d 单事件最大=%dus 接收速率=%s' % (
                  post_seconds, post['overflow_warnings'],
                  post['fifo_ovf_events'], post['data_events'], post['rx_ovf_last'],
                  post['rx_pend_peak_max'], post['queue_peak_max'],
                  post['reset_dropped_last'], post['break_events'],
                  post['handle_max_us'],
                  'n/a' if rate is None else '%.1f KB/s' % (rate / 1024.0)))
        if post['overflow_times_ms']:
            print('溢出时刻(相对 t=0)：%s' % post['overflow_times_ms'])
        extra = post.get('uart_extra') or {}
        if extra:
            print('UART1 累计口径：%s' % ' '.join(
                '%s=%d' % (key, extra[key]) for key in sorted(extra)))
    delta = report.get('M_report_delta')
    rate_m = None if delta is None else delta / post_seconds
    print('M 侧报告增量：%s（下限 %d，平均 %s/s）' % (
        delta, min_reports, 'n/a' if rate_m is None else '%.0f' % rate_m))

    for label in ('P', 'M'):
        if label == report.get('reset_label'):
            after = report.get(label + '_queues_after_reset', {})
            # print_queues 的 delta 参数需要 peak_abs 字段：复位后直接用累计值本身充当。
            as_delta = {name: {'received': q['received'], 'rejected': q['rejected'],
                               'dropped': q['dropped'], 'peak_abs': q['peak']}
                        for name, q in after.items()}
            print_queues('%s（窗口起点被复位：显示复位后累计与峰值）' % label,
                         after, as_delta)
        else:
            print_queues('%s（显示统计窗口内增量）' % label,
                         report.get(label + '_queues_full', {}),
                         report.get(label + '_queues_post_delta', {}))
    print_counters('P', report.get('P_counters_full', {}),
                   report.get('P_counters_post_delta', {}))
    print_counters('M', report.get('M_counters_full', {}),
                   report.get('M_counters_post_delta', {}))
    print('A/B 条件（开机暂停写盘标记）：%s' % (
        '已生效' if report['conditions']['paused_condition_seen'] else '未观察到'))
    print('判定：%s' % report['verdict'])


def main():
    parser = argparse.ArgumentParser(description='UART1 写盘暂停 A/B 编排')
    parser.add_argument('--ports', help='两个 UART0 端口，如 COM3,COM13')
    parser.add_argument('--labels', default='P,M', help='与端口对应的标签（缺省 P,M）')
    parser.add_argument('--reset-label', default='M', help='测试动作复位哪一块（缺省 M）')
    parser.add_argument('--baud', type=int, default=BAUD_DEFAULT)
    parser.add_argument('--quiet-seconds', type=float, default=10.0,
                        help='清计数后的静默稳定期（缺省 10 秒）')
    parser.add_argument('--pre-seconds', type=float, default=3.0,
                        help='复位 M 之前的基线窗口（缺省 3 秒）')
    parser.add_argument('--post-seconds', type=float, default=20.0,
                        help='复位 M 之后的统计窗口（缺省 20 秒）')
    parser.add_argument('--min-reports', type=int, default=500,
                        help='判定负载有效的 M 侧报告数下限')
    parser.add_argument('--no-notify', action='store_true', help='不响铃')
    parser.add_argument('--outdir', default='artifacts/tests')
    parser.add_argument('--tag', default='uart1-paused-log-ab')
    parser.add_argument('--analyze-only', action='store_true',
                        help='不占用串口，只按已有 summary.json 复算判定')
    parser.add_argument('--summary', default='',
                        help='配合 --analyze-only：已生成的 summary.json 路径')
    args = parser.parse_args()

    if args.analyze_only:
        if not args.summary:
            parser.error('--analyze-only 需要 --summary <summary.json>')
        with open(args.summary, 'r', encoding='utf-8') as handle:
            previous = json.load(handle)
        reset_at = float(previous['reset_at_s'])
        pre_at = float(previous['window_pre_s'][0])
        post_seconds = float(previous['window_post_s'][1]) - reset_at
        report = build_report(previous.get('tag', args.tag),
                              previous.get('stamp', 'recalc'),
                              previous.get('reset_label', args.reset_label),
                              reset_at, pre_at, post_seconds, args.min_reports,
                              previous.get('logs', {}), previous.get('ports', {}))
        with open(args.summary, 'w', encoding='utf-8') as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
        print_summary(report, post_seconds, args.min_reports)
        print('已复算并覆盖：%s' % args.summary)
        return 0

    if not args.ports:
        parser.error('采集模式需要 --ports')
    ports = [item.strip() for item in args.ports.split(',') if item.strip()]
    labels = [item.strip() for item in args.labels.split(',') if item.strip()]
    if len(ports) < 2 or len(labels) != len(ports):
        parser.error('需要两个端口，且 --labels 数量与之一致')
    if args.reset_label not in labels:
        parser.error('--reset-label 必须是 --labels 之一')

    os.makedirs(args.outdir, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    start = time.monotonic()

    recorders = []
    log_paths = {}
    for label, port in zip(labels, ports):
        path = os.path.join(args.outdir, 'dual-serial-%s-%s-%s.log' % (
            label, stamp, args.tag))
        recorder = PortRecorder(label, port, args.baud, path, start,
                                start + args.quiet_seconds + args.pre_seconds +
                                args.post_seconds + 60.0)
        recorders.append(recorder)
        log_paths[label] = path
        recorder.start()

    for recorder in recorders:
        recorder.opened.wait(5.0)
    for recorder in recorders:
        print('%s=%s -> %s' % (recorder.label, recorder.port_name, recorder.path))
    if any(recorder.port is None for recorder in recorders):
        print('错误：有串口未能打开：%s' % ', '.join(
            '%s(%s)=%s' % (r.label, r.port_name, r.open_error)
            for r in recorders if r.port is None), file=sys.stderr)
        for recorder in recorders:
            recorder.stop_requested.set()
        return 2

    def wait_until(offset_seconds, note):
        target = start + offset_seconds
        while True:
            remaining = target - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(0.2, remaining))
        print('[+%.2fs] %s' % (time.monotonic() - start, note), flush=True)

    # 阶段 1：依次硬复位两板 = 清空 RAM 统计计数（协议里没有清计数命令）。
    for recorder in recorders:
        if recorder.hard_reset():
            print('[+%.2fs] 清计数复位 %s (%s)' % (
                time.monotonic() - start, recorder.label, recorder.port_name),
                flush=True)
        time.sleep(1.5)
    quiet_at = time.monotonic() - start

    # 阶段 2：静默稳定期（留给身份协商、Profile 采集与克隆重建）。
    wait_until(quiet_at + args.quiet_seconds, '静默期结束')

    # 阶段 3：提示操作者准备并开始持续快速移动。
    if not args.no_notify:
        notify('UART1 A/B 测试：请立刻开始持续快速移动鼠标约 %d 秒'
               % int(args.post_seconds))
        print('[+%.2fs] 已响铃提示：请持续快速移动鼠标' % (
            time.monotonic() - start), flush=True)
        time.sleep(2.0)

    pre_at = time.monotonic() - start
    wait_until(pre_at + args.pre_seconds, '基线窗口结束')

    # 阶段 4：复位 M（测试动作），统计窗口从此刻开始。
    target = next(recorder for recorder in recorders
                  if recorder.label == args.reset_label)
    reset_ok = target.hard_reset()
    reset_at = time.monotonic() - start
    print('[+%.2fs] %s 复位%s' % (reset_at, args.reset_label,
                                 '' if reset_ok else '失败'), flush=True)

    # 阶段 5：统计窗口。
    wait_until(reset_at + args.post_seconds, '统计窗口结束')

    for recorder in recorders:
        recorder.stop_requested.set()
    for recorder in recorders:
        recorder.join(timeout=5.0)

    report = build_report(args.tag, stamp, args.reset_label, reset_at, pre_at,
                          args.post_seconds, args.min_reports, log_paths,
                          dict(zip(labels, ports)))
    summary_path = os.path.join(args.outdir, 'summary-%s-%s.json' % (
        stamp, args.tag))
    with open(summary_path, 'w', encoding='utf-8') as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)

    print_summary(report, args.post_seconds, args.min_reports)
    print('摘要文件：%s' % summary_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
