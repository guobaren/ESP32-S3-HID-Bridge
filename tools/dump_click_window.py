#!/usr/bin/env python3
"""按时间轴摊开一段窗口内的 RX / SUB / COMP 鼠标事件，用于逐帧核对按钮时序。

用途：解释"SUB 侧结构上夹零、但逐帧与 RX 状态不矛盾"这类现象——
把同一时刻三层各自的按钮值并排摆出来，看零到底出现在 RX 的哪个位置。

用法：
    python tools/dump_click_window.py <jsonl> [--around auto] [--span-ms 40]
"""

from __future__ import annotations

import argparse
import json
from typing import Dict, List, Tuple

UINT32 = 0xFFFFFFFF
MOUSE_INTERFACE = 1
MOUSE_REPORT_ID = 2
MOUSE_BODY_BYTES = 8
Event = Tuple[int, int, bytes]  # (event_id, ts, body)


def load(path: str):
    rx: Dict[int, Event] = {}
    sub: Dict[int, Event] = {}
    comp: Dict[int, Event] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"kind_name"' not in line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("record_type") != "diag_event":
                continue
            kind = record.get("kind_name")
            try:
                raw = bytes.fromhex(record.get("data_hex") or "")
            except ValueError:
                continue
            event_id = int(record.get("event_id", 0))
            ts = int(record.get("timestamp_us_low32", 0))
            if kind == "RAW_HID_INPUT" and len(raw) >= 11 and \
                    raw[0] == MOUSE_INTERFACE and raw[1] == MOUSE_REPORT_ID and \
                    raw[2] == MOUSE_BODY_BYTES:
                rx[event_id] = (event_id, ts, raw[3:11])
            elif kind == "P_USB_REPORT_SUBMIT" and len(raw) >= 12 and \
                    raw[0] == 1 and raw[1] == 1 and raw[2] == MOUSE_REPORT_ID and \
                    raw[3] == MOUSE_BODY_BYTES:
                sub[event_id] = (event_id, ts, raw[4:12])
            elif kind == "P_USB_REPORT_COMPLETE" and len(raw) >= 3:
                declared = raw[1]
                body = raw[2:2 + declared] if declared else raw[2:]
                if len(body) == MOUSE_BODY_BYTES + 1 and body[0] == MOUSE_REPORT_ID:
                    body = body[1:]
                if len(body) == MOUSE_BODY_BYTES:
                    comp[event_id] = (event_id, ts, body)
    return rx, sub, comp


def first_submit_zero_in_press(sub: Dict[int, Event], merge_us: int = 25000):
    """找 SUB 序列中第一个"夹在非零之间"的零报告。"""
    ids = sorted(sub)
    for position in range(1, len(ids) - 1):
        if sub[ids[position]][2][0] != 0:
            continue
        if sub[ids[position - 1]][2][0] == 0:
            continue
        # 向后找下一个非零
        for probe in range(position + 1, len(ids)):
            if sub[ids[probe]][2][0] != 0:
                gap = (sub[ids[probe]][1] - sub[ids[position]][1]) & UINT32
                if gap <= merge_us:
                    return ids[position]
                break
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("jsonl")
    parser.add_argument("--span-ms", type=float, default=40.0)
    args = parser.parse_args()

    rx, sub, comp = load(args.jsonl)
    target = first_submit_zero_in_press(sub)
    if target is None:
        print("未找到 SUB 侧夹零段")
        return 0

    centre = sub[target][1]
    span = int(args.span_ms * 1000)
    print("样例中心：SUB event_id=%d（提交了一份 buttons=0），窗口 ±%.0f ms"
          % (target, args.span_ms))
    print()
    print("  相对ms     来源  event_id  按钮  报告内容          备注")
    print("  --------   ----  --------  ----  ----------------  ----")

    rows: List[Tuple[int, str, int, int, bytes]] = []
    for table, tag in ((rx, "RX"), (sub, "SUB"), (comp, "COMP")):
        for event_id, ts, body in table.values():
            delta = (ts - centre) & UINT32
            if delta > 0x80000000:
                delta -= 0x100000000
            if -span <= delta <= span:
                rows.append((delta, tag, event_id, body[0], body))
    rows.sort()

    # 逐行输出，并标注每个 SUB 零报告时刻的"RX 最新值"
    for delta, tag, event_id, buttons, body in rows:
        note = ""
        if tag == "SUB" and buttons == 0:
            ts = (centre + delta) & UINT32
            latest = None
            for rid in sorted(rx):
                if rx[rid][1] <= ts:
                    latest = rx[rid]
                else:
                    break
            if latest is not None:
                note = "RX 此刻最新 = id=%d buttons=%d" % (latest[0], latest[2][0])
        if event_id == target:
            note = (note + "  <<< 样例零报告").strip()
        print("  %+9.3f  %-4s  %-8d  %-4d  %-16s  %s"
              % (delta / 1000.0, tag, event_id, buttons, body.hex(), note))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
