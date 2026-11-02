#!/usr/bin/env python3
"""判定按钮回落在哪一层被引入：只比较鼠标接口的 8 字节报告，逐帧核对按钮状态。

三层事件（firmware/main/pc_hid_output.c 埋点）：
  RAW_HID_INPUT         [interface][report_id][length][body...]
  P_USB_REPORT_SUBMIT   [submitted][instance][report_id][length][body...]
  P_USB_REPORT_COMPLETE [instance][length][report_id][body...]   ← 回调给的 report 含 report ID

核心判据（不需要内容匹配，最干净）：
  对 SUBMIT 提交的每一份按钮=0 报告，取它在 RX 序列中"时间上最近的上一份报告"的按钮值。
    * 若 RX 当时是"按下" -> 这份 0 与 M 送来的当前状态矛盾 => 由 P 端提交顺序引入。
    * 若 RX 当时是 0     -> 与 M 送来状态一致，属正常透传。
  反向同理：SUBMIT 提交按钮!=0 而 RX 当时是 0，也是一次矛盾（多余的按下）。

用法：
    python tools/trace_click_dup.py <jsonl> --samples 3 --out report.txt
"""

from __future__ import annotations

import argparse
import bisect
import json
import sys
from collections import Counter
from typing import Dict, List, Optional, Tuple

UINT32 = 0xFFFFFFFF
MOUSE_INTERFACE = 1
MOUSE_REPORT_ID = 2
MOUSE_BODY_BYTES = 8

Event = Tuple[int, bytes]  # (timestamp_us_low32, body)


def delta_us(later: int, earlier: int) -> int:
    return (later - earlier) & UINT32


def load(path: str):
    """只保留鼠标接口的 8 字节报告，避免厂商接口把窗口搅浑。"""
    rx: Dict[int, Event] = {}
    sub: Dict[int, Event] = {}
    comp: Dict[int, Event] = {}
    skipped: Counter = Counter()
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line or '"kind_name"' not in line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("record_type") != "diag_event":
                continue
            kind = record.get("kind_name")
            if kind not in ("RAW_HID_INPUT", "P_USB_REPORT_SUBMIT", "P_USB_REPORT_COMPLETE"):
                continue
            try:
                raw = bytes.fromhex(record.get("data_hex") or "")
            except ValueError:
                continue
            event_id = int(record.get("event_id", 0))
            ts = int(record.get("timestamp_us_low32", 0))

            if kind == "RAW_HID_INPUT":
                if len(raw) >= 3 + MOUSE_BODY_BYTES and raw[0] == MOUSE_INTERFACE \
                        and raw[1] == MOUSE_REPORT_ID and raw[2] == MOUSE_BODY_BYTES:
                    rx[event_id] = (ts, raw[3:3 + MOUSE_BODY_BYTES])
                else:
                    skipped["rx_non_mouse"] += 1
            elif kind == "P_USB_REPORT_SUBMIT":
                if len(raw) >= 4 + MOUSE_BODY_BYTES and raw[0] == 1 and raw[1] == 1 \
                        and raw[2] == MOUSE_REPORT_ID and raw[3] == MOUSE_BODY_BYTES:
                    sub[event_id] = (ts, raw[4:4 + MOUSE_BODY_BYTES])
                else:
                    skipped["sub_non_mouse"] += 1
            else:
                declared = raw[1] if len(raw) >= 2 else 0
                body = raw[2:2 + declared] if declared else raw[2:]
                if len(body) == MOUSE_BODY_BYTES + 1 and body[0] == MOUSE_REPORT_ID:
                    body = body[1:]
                if len(body) == MOUSE_BODY_BYTES:
                    comp[event_id] = (ts, body)
                else:
                    skipped["comp_non_mouse"] += 1
    return rx, sub, comp, skipped


def buttons_of(body: bytes) -> int:
    return body[0] if body else -1


def press_runs(ids: List[int], table: Dict[int, Event], merge_us: int):
    runs = []
    index = 0
    while index < len(ids):
        if buttons_of(table[ids[index]][1]) == 0:
            index += 1
            continue
        start = index
        last_nonzero = index
        inner: List[int] = []
        cursor = index
        while cursor < len(ids):
            if buttons_of(table[ids[cursor]][1]) != 0:
                last_nonzero = cursor
                cursor += 1
                continue
            probe = cursor
            while probe < len(ids) and buttons_of(table[ids[probe]][1]) == 0:
                probe += 1
            if probe >= len(ids):
                inner.extend(ids[cursor:probe])
                cursor = probe
                break
            if delta_us(table[ids[probe]][0], table[ids[cursor]][0]) > merge_us:
                break
            inner.extend(ids[cursor:probe])
            cursor = probe
        runs.append({"first": ids[start], "last": ids[last_nonzero], "inner": inner})
        index = max(cursor, start + 1)
    return runs


def classify(out, name: str, ids: List[int], table: Dict[int, Event],
             rx_ts: List[int], rx_table: List[Event]) -> Tuple[int, int]:
    """按"RX 当时状态"给每个提交样本分类，返回 (矛盾数, 样本数)。"""
    mismatch_zero = 0
    mismatch_press = 0
    taken = 0
    for event_id in ids:
        ts, body = table[event_id]
        if not body:
            continue
        position = bisect.bisect_right(rx_ts, ts) - 1
        if position < 0:
            continue
        taken += 1
        rx_buttons = buttons_of(rx_table[position][1])
        if body[0] == 0 and rx_buttons != 0:
            mismatch_zero += 1
        elif body[0] != 0 and rx_buttons == 0:
            mismatch_press += 1
    out("── %s ──" % name)
    out("  样本数                    : %d" % len(ids))
    out("  可与 RX 对齐的样本        : %d" % taken)
    out("  与 RX 当时状态矛盾的零报告: %d   ← P 端在按住期间提交了 0" % mismatch_zero)
    out("  与 RX 当时状态矛盾的非零  : %d   ← P 端在松开期间提交了按下" % mismatch_press)
    out()
    return mismatch_zero, mismatch_press


def dump_window(out, rx, sub, comp, first: int, last: int, mark: int, span: int) -> None:
    low, high = first - span, last + span
    out("  窗口 id=%d..%d（RX=到达 / SUB=提交 / COMP=PC取走）" % (low, high))
    for event_id in range(low, high + 1):
        if event_id in rx:
            tag, ts, body = "RX  ", rx[event_id][0], rx[event_id][1]
        elif event_id in sub:
            tag, ts, body = "SUB ", sub[event_id][0], sub[event_id][1]
        elif event_id in comp:
            tag, ts, body = "COMP", comp[event_id][0], comp[event_id][1]
        else:
            continue
        flag = "   <<< 矛盾零报告" if event_id == mark else ""
        out("    id=%-7d %s buttons=%d body=%s%s"
            % (event_id, tag, buttons_of(body), body.hex(), flag))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="判定按钮回落在哪一层被引入")
    parser.add_argument("jsonl")
    parser.add_argument("--samples", type=int, default=2)
    parser.add_argument("--span", type=int, default=5)
    parser.add_argument("--merge-us", type=int, default=25000)
    parser.add_argument("--out")
    args = parser.parse_args()

    handle = open(args.out, "w", encoding="utf-8") if args.out else None
    lines: List[str] = []

    def out(text: str = "") -> None:
        lines.append(text)
        if handle is not None:
            handle.write(text + "\n")

    rx, sub, comp, skipped = load(args.jsonl)
    rx_ids, sub_ids, comp_ids = sorted(rx), sorted(sub), sorted(comp)
    out("数据：%s" % args.jsonl)
    out("非鼠标报告已排除：%s" % dict(skipped))
    out()

    rx_runs = press_runs(rx_ids, rx, args.merge_us)
    sub_runs = press_runs(sub_ids, sub, args.merge_us)
    comp_runs = press_runs(comp_ids, comp, args.merge_us)
    out("═══ 按住段结构（鼠标接口 8 字节报告）═══")
    for name, runs in (("M 送来(RX)", rx_runs), ("P 提交(SUB)", sub_runs),
                       ("PC 取走(COMP)", comp_runs)):
        dirty = [r for r in runs if r["inner"]]
        out("  %-14s 段数=%-4d 内部夹零=%-4d (%.1f%%)"
            % (name, len(runs), len(dirty),
               100.0 * len(dirty) / len(runs) if runs else 0.0))
    out()

    rx_sorted: List[Event] = [rx[i] for i in rx_ids]
    rx_ts = [item[0] for item in rx_sorted]
    out("═══ 与 RX 当时状态的矛盾统计 ═══")
    sub_zero, sub_press = classify(out, "P 提交 (P_USB_REPORT_SUBMIT)",
                                   sub_ids, sub, rx_ts, rx_sorted)
    comp_zero, comp_press = classify(out, "PC 取走 (P_USB_REPORT_COMPLETE)",
                                     comp_ids, comp, rx_ts, rx_sorted)

    # 展开"RX 当时为按下、却被提交成 0"的具体样例
    dirty = [r for r in sub_runs if r["inner"]]
    out("═══ SUBMIT 侧夹零段样例 ═══")
    shown = 0
    for run in dirty:
        if shown >= args.samples:
            break
        for zero_id in run["inner"]:
            ts, body = sub[zero_id]
            position = bisect.bisect_right(rx_ts, ts) - 1
            rx_state = buttons_of(rx_sorted[position][1]) if position >= 0 else -1
            if rx_state == 0:
                continue  # 与 RX 一致，不是矛盾样例
            out()
            out("[样例 %d] SUBMIT 在 id=%d 提交 buttons=0，但 RX 当时最近的状态是 buttons=%d"
                % (shown + 1, zero_id, rx_state))
            out("  RX 最近报告 id 时间=%d，SUB 提交时间=%d，相差 %d us"
                % (rx_sorted[position][0], ts, delta_us(ts, rx_sorted[position][0])))
            out("  矛盾零报告 body=%s（RX 中此前最近的 zero body=%s）"
                % (body.hex(), rx_sorted[position][1].hex()))
            dump_window(out, rx, sub, comp, run["first"], run["last"], zero_id, args.span)
            shown += 1
            break
    if shown == 0:
        out("  （没有找到「RX 按下却被提交成 0」的样例）")

    out()
    out("═══ 判定 ═══")
    out("  RX  内部夹零段 : %d" % len([r for r in rx_runs if r["inner"]]))
    out("  SUB 内部夹零段 : %d，其中与 RX 状态矛盾 %d" % (
        len(dirty), sub_zero))
    out("  COMP 内部夹零段: %d，其中与 RX 状态矛盾 %d" % (
        len([r for r in comp_runs if r["inner"]]), comp_zero))
    if handle is not None:
        handle.close()
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
