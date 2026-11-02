#!/usr/bin/env python3
"""分析 P 板 DIAG_STREAM 记录，定位"一次物理点击被上报成两次按下"。

数据来源：tools/dual_uart_inspect.py 采集的 *.jsonl（record_type=diag_event）。

事件布局（firmware/main/pc_hid_output.c 的 diag_capture_* 埋点）：
  P_USB_REPORT_SUBMIT  : [submitted][instance][report_id][length][report...]
  P_USB_REPORT_COMPLETE: [instance][length][report...]
  RAW_HID_INPUT        : [interface][report_id][length][report...]  ← UART1 收到的 M 侧原始报告

鼠标报告正文首字节是按钮位：M 侧 hid_host_mouse.c 用 event.data[0] 判"按键边沿"，
P 侧 hid_report_layout 解析出的 buttons_bit_offset 同为 0。

判读：
  * "回落"（bounce）= 非零 → 零(短暂) → 非零。按住期间的这种回落，在 PC 看来就是
    一次多余的"释放+按下"，即单击被拆成两次。
  * 关键对照：RAW_HID_INPUT（M 送来什么）与 P_USB_REPORT_SUBMIT（P 发给 PC 什么）。
    若 RX 干净而 SUBMIT 出现回落，则抖动由 P 端引入；若两侧相同则是 M 侧/物理侧。
  * 诊断流存在丢弃（dropped>0），因此"未观测到回落"不能作为否定证据，计数是下界。

用法：
    python tools/analyze_click_dup.py artifacts/tests/<dir>/<tag>.jsonl --out report.txt
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass
from typing import List, Optional

UINT32 = 0xFFFFFFFF
VALID_NOTES = {
    "reassembly": "分片重组错误",
}


@dataclass
class Sample:
    event_id: int
    ts_us: int
    report: bytes
    origin: int  # instance（SUBMIT/COMPLETE）或 interface（RAW_HID_INPUT）
    report_id: int

    @property
    def buttons(self) -> int:
        return self.report[0] if self.report else 0

    def hex(self) -> str:
        return self.report.hex()


class Report:
    """把结果同时写控制台和 UTF-8 文件。"""

    def __init__(self, path: Optional[str]):
        self.lines: List[str] = []
        self.handle = open(path, "w", encoding="utf-8") if path else None

    def __call__(self, text: str = "") -> None:
        self.lines.append(text)
        if self.handle is not None:
            self.handle.write(text + "\n")

    def close(self) -> None:
        if self.handle is not None:
            self.handle.close()
            self.handle = None


def iter_events(path: str):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("record_type") == "diag_event":
                yield record


def collect(path: str):
    submits: List[Sample] = []
    raw_inputs: List[Sample] = []
    completes: List[Sample] = []
    counts: Counter = Counter()
    truncated = 0
    id_gaps: Counter = Counter()
    previous_id: Optional[int] = None

    for record in iter_events(path):
        kind = record.get("kind_name") or "kind_%s" % record.get("kind")
        counts[kind] += 1
        if record.get("underlying_payload_truncated"):
            truncated += 1

        event_id = int(record.get("event_id", 0))
        if previous_id is not None:
            missing = event_id - previous_id - 1
            if missing > 0:
                id_gaps[missing] += 1
        previous_id = event_id

        try:
            raw = bytes.fromhex(record.get("data_hex") or "")
        except ValueError:
            continue
        ts_us = int(record.get("timestamp_us_low32", 0))

        if kind == "P_USB_REPORT_SUBMIT" and len(raw) >= 5:
            if raw[0] != 1:
                continue
            submits.append(Sample(event_id, ts_us, raw[4:], raw[1], raw[2]))
        elif kind == "RAW_HID_INPUT" and len(raw) >= 4:
            raw_inputs.append(Sample(event_id, ts_us, raw[3:], raw[0], raw[1]))
        elif kind == "P_USB_REPORT_COMPLETE" and len(raw) >= 3:
            completes.append(Sample(event_id, ts_us, raw[2:], raw[0], -1))

    return submits, raw_inputs, completes, counts, truncated, id_gaps


def delta_us(later: int, earlier: int) -> int:
    return (later - earlier) & UINT32


def find_bounces(samples: List[Sample], hold_us: int):
    """返回 (按下起点, 首个零样本, 恢复样本, 零样本数, 零段时长us) 列表。"""
    bounces = []
    index = 0
    total = len(samples)
    while index < total:
        if samples[index].buttons == 0:
            index += 1
            continue
        start = index
        cursor = index + 1
        while cursor < total and samples[cursor].buttons != 0:
            cursor += 1
        if cursor >= total:
            break
        zero_start = cursor
        while cursor < total and samples[cursor].buttons == 0:
            cursor += 1
        if cursor >= total:
            break
        duration = delta_us(samples[cursor].ts_us, samples[zero_start].ts_us)
        if duration <= hold_us:
            bounces.append((samples[start], samples[zero_start],
                            samples[cursor], cursor - zero_start, duration))
        index = cursor
    return bounces


def press_episodes(samples: List[Sample], merge_us: int):
    """把按钮流切成"物理按住"段；间隔不超过 merge_us 的零段视为段内抖动。"""
    episodes = []
    index = 0
    total = len(samples)
    while index < total:
        if samples[index].buttons == 0:
            index += 1
            continue
        start = index
        cursor = index
        last_nonzero = index
        inner_zeros: List[Sample] = []
        while cursor < total:
            if samples[cursor].buttons != 0:
                last_nonzero = cursor
                cursor += 1
                continue
            probe = cursor
            while probe < total and samples[probe].buttons == 0:
                probe += 1
            if probe >= total:
                inner_zeros.extend(samples[cursor:probe])
                cursor = probe
                break
            if delta_us(samples[probe].ts_us, samples[cursor].ts_us) > merge_us:
                break
            inner_zeros.extend(samples[cursor:probe])
            cursor = probe
        episodes.append({
            "first": samples[start],
            "last": samples[last_nonzero],
            "inner_zeros": inner_zeros,
        })
        index = max(cursor, start + 1)
    return episodes


def byte_field_profile(samples: List[Sample], byte_index: int) -> Counter:
    profile: Counter = Counter()
    for sample in samples:
        if len(sample.report) > byte_index:
            profile[sample.report[byte_index]] += 1
    return profile


def describe(out: Report, name: str, samples: List[Sample], hold_us: int) -> None:
    out("── %s ──" % name)
    out("  样本数            : %d" % len(samples))
    if not samples:
        out()
        return
    nonzero = sum(1 for s in samples if s.buttons != 0)
    out("  按钮非零样本      : %d (%.2f%%)" % (nonzero, 100.0 * nonzero / len(samples)))
    span = delta_us(samples[-1].ts_us, samples[0].ts_us) / 1e6
    out("  板内时间跨度      : %.1f s" % span)
    out("  平均样本率        : %.0f /s" % (len(samples) / span if span > 0 else 0))
    out("  byte0 取值分布    : %s" % dict(byte_field_profile(samples, 0).most_common(6)))
    out("  byte1 取值分布    : %s" % dict(byte_field_profile(samples, 1).most_common(6)))

    episodes = press_episodes(samples, hold_us)
    contaminated = [e for e in episodes if e["inner_zeros"]]
    out("  按住段数          : %d" % len(episodes))
    out("  含内部零样本的段  : %d (%.2f%%)" % (
        len(contaminated),
        100.0 * len(contaminated) / len(episodes) if episodes else 0.0))

    bounces = find_bounces(samples, hold_us)
    out("  回落(1-0-1)次数   : %d  [零段 <= %d us]" % (len(bounces), hold_us))
    for press, zero, resume, zeros, duration in bounces[:20]:
        out("     按下 id=%d -> 回落 id=%d (零 %d 个, %d us) -> 恢复 id=%d"
            % (press.event_id, zero.event_id, zeros, duration, resume.event_id))
    if len(bounces) > 20:
        out("     ... 其余 %d 次省略" % (len(bounces) - 20))
    out()


def context_dump(out: Report, title: str, samples: List[Sample],
                 episodes, radius: int = 3) -> None:
    """把有内部零样本的按住段连同前后文dump出来，便于逐帧核对。"""
    hit = [e for e in episodes if e["inner_zeros"]]
    out("── %s：含内部零样本的段明细（共 %d 段）──" % (title, len(hit)))
    position = {id(s): i for i, s in enumerate(samples)}
    for order, episode in enumerate(hit, 1):
        first = episode["first"]
        last = episode["last"]
        start = max(0, position[id(first)] - radius)
        end = min(len(samples), position[id(last)] + radius + 1)
        inner_ids = ",".join(str(s.event_id) for s in episode["inner_zeros"])
        out("  [段 %d] 按下 id=%d .. 释放 id=%d；内部零样本 id=%s（%d 个）"
            % (order, first.event_id, last.event_id, inner_ids,
               len(episode["inner_zeros"])))
        for sample in samples[start:end]:
            marker = "  <-- 零" if sample in episode["inner_zeros"] else ""
            out("      id=%-7d ts=%-12d buttons=%d report=%s%s"
                % (sample.event_id, sample.ts_us, sample.buttons,
                   sample.hex(), marker))
        out()


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="定位单击被拆成两次的提交序列证据")
    parser.add_argument("jsonl", help="dual_uart_inspect.py 产出的 .jsonl")
    parser.add_argument("--out", help="同时写入该 UTF-8 文本文件")
    parser.add_argument("--hold-us", type=int, default=25000,
                        help="判定为瞬时回落的零段最大时长（微秒）")
    args = parser.parse_args()

    out = Report(args.out)
    submits, raw_inputs, completes, counts, truncated, id_gaps = collect(args.jsonl)

    out("数据文件：%s" % args.jsonl)
    out("事件统计：")
    for kind, count in counts.most_common():
        out("  %-24s %d" % (kind, count))
    out("  载荷被截断的记录  : %d" % truncated)
    if id_gaps:
        missing_total = sum(gap * times for gap, times in id_gaps.items())
        out("  event_id 跳号     : %d 处，累计缺失约 %d（诊断流丢弃）"
            % (sum(id_gaps.values()), missing_total))
    out()

    describe(out, "M 送来 (RAW_HID_INPUT / UART1_RX)", raw_inputs, args.hold_us)
    describe(out, "P 发给 PC (P_USB_REPORT_SUBMIT)", submits, args.hold_us)
    describe(out, "PC 取走 (P_USB_REPORT_COMPLETE)", completes, args.hold_us)

    input_episodes = press_episodes(raw_inputs, args.hold_us)
    submit_episodes = press_episodes(submits, args.hold_us)
    context_dump(out, "M 送来 (RX)", raw_inputs, input_episodes)
    context_dump(out, "P 发给 PC (SUBMIT)", submits, submit_episodes)

    submit_bounces = find_bounces(submits, args.hold_us)
    input_bounces = find_bounces(raw_inputs, args.hold_us)
    out("═══ 判定 ═══")
    out("  RX 侧回落次数     : %d" % len(input_bounces))
    out("  SUBMIT 侧回落次数 : %d" % len(submit_bounces))
    if submit_bounces and not input_bounces:
        out("  -> 回落只出现在 SUBMIT 侧：抖动由 P 端引入（M 送来序列干净）")
    elif submit_bounces and input_bounces:
        out("  -> 两侧都有回落：需逐段核对是否同一物理事件被放大")
    elif not submit_bounces and not input_bounces:
        out("  -> 两侧都未见回落：本窗口未复现，或回落落在被丢弃样本中")
    else:
        out("  -> 异常组合，人工复核")
    if truncated:
        out("  注意：存在被截断载荷，按钮判断可能不完整")
    if id_gaps:
        out("  注意：诊断流丢弃了样本，以上计数是下界，不能据此否定未观测到的回落")
    out.close()

    for line in out.lines:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
