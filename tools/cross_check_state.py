#!/usr/bin/env python3
"""交叉核对「提交侧是否在按住期间提交了未按下」，并先验证判据前提。

背景：dual_diag_stream_record 是"先原子取 event_id、再读 esp_timer 时间戳"，
多任务/中断并发时 event_id 顺序与时间戳顺序未必一致。因此：
  * id 判据：用 event_id 顺序回看"此前最后一条 RX 报告的按钮值"（不依赖时间戳）
  * ts 判据：用时间戳二分回看（依赖时间戳单调）
两者都输出，并先报告时间戳回退次数，用来判断哪种判据可信。

用法：python tools/cross_check_state.py <jsonl>
"""

from __future__ import annotations

import argparse
import bisect
import json

MOUSE_INTERFACE = 1
MOUSE_REPORT_ID = 2
MOUSE_BODY_BYTES = 8


def load(path: str):
    rx, sub, comp = {}, {}, {}
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
                rx[event_id] = (ts, raw[3:11])
            elif kind == "P_USB_REPORT_SUBMIT" and len(raw) >= 12 and \
                    raw[0] == 1 and raw[1] == 1 and raw[2] == MOUSE_REPORT_ID and \
                    raw[3] == MOUSE_BODY_BYTES:
                sub[event_id] = (ts, raw[4:12])
            elif kind == "P_USB_REPORT_COMPLETE" and len(raw) >= 3:
                declared = raw[1]
                body = raw[2:2 + declared] if declared else raw[2:]
                if len(body) == MOUSE_BODY_BYTES + 1 and body[0] == MOUSE_REPORT_ID:
                    body = body[1:]
                if len(body) == MOUSE_BODY_BYTES:
                    comp[event_id] = (ts, body)
    return rx, sub, comp


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("jsonl")
    args = parser.parse_args()

    rx, sub, comp = load(args.jsonl)
    rx_ids, sub_ids, comp_ids = sorted(rx), sorted(sub), sorted(comp)
    print("样本：RX=%d  SUB=%d  COMP=%d" % (len(rx_ids), len(sub_ids), len(comp_ids)))

    print("\n[前提检查] 按 event_id 顺序，时间戳回退（未单调递增）次数：")
    for name, ids, table in (("RX", rx_ids, rx), ("SUB", sub_ids, sub),
                             ("COMP", comp_ids, comp)):
        series = [table[i][0] for i in ids]
        back = sum(1 for i in range(1, len(series)) if series[i] < series[i - 1])
        span = (series[-1] - series[0]) if series else 0
        print("  %-4s 回退 %d 次 / 共 %d 条（跨度 %d us）"
              % (name, back, len(series), span))

    rx_ts = [rx[i][0] for i in rx_ids]

    print("\n[矛盾统计] 提交时与 RX 当前按钮状态不一致的次数：")
    for name, ids, table in (("SUB", sub_ids, sub), ("COMP", comp_ids, comp)):
        id_zero = id_press = ts_zero = ts_press = 0
        for event_id in ids:
            ts, body = table[event_id]
            position = bisect.bisect_left(rx_ids, event_id) - 1
            if position >= 0:
                rx_buttons = rx[rx_ids[position]][1][0]
                if body[0] == 0 and rx_buttons != 0:
                    id_zero += 1
                elif body[0] != 0 and rx_buttons == 0:
                    id_press += 1
            tpos = bisect.bisect_right(rx_ts, ts) - 1
            if tpos >= 0:
                rx_buttons = rx[rx_ids[tpos]][1][0]
                if body[0] == 0 and rx_buttons != 0:
                    ts_zero += 1
                elif body[0] != 0 and rx_buttons == 0:
                    ts_press += 1
        print("  %-4s [id判据] 按住期间提交0(=幽灵释放) = %-4d  松开期间提交非零 = %d"
              % (name, id_zero, id_press))
        print("  %-4s [ts判据] 按住期间提交0(=幽灵释放) = %-4d  松开期间提交非零 = %d"
              % (name, ts_zero, ts_press))

    # 结构判据：不依赖任何跨层对齐，只看各层自身序列
    def runs(ids, table, merge_us=25000):
        out_runs = []
        i = 0
        while i < len(ids):
            if table[ids[i]][1][0] == 0:
                i += 1
                continue
            inner = 0
            cursor = i
            while cursor < len(ids):
                if table[ids[cursor]][1][0] != 0:
                    cursor += 1
                    continue
                probe = cursor
                while probe < len(ids) and table[ids[probe]][1][0] == 0:
                    probe += 1
                if probe >= len(ids):
                    break
                if (table[ids[probe]][0] - table[ids[cursor]][0]) & 0xFFFFFFFF > merge_us:
                    break
                inner += probe - cursor
                cursor = probe
            out_runs.append(inner)
            i = max(cursor, i + 1)
        return out_runs

    print("\n[结构判据] 各层自身的「按住段」与「段内夹零」——不依赖任何跨层对齐：")
    for name, ids, table in (("RX", rx_ids, rx), ("SUB", sub_ids, sub),
                             ("COMP", comp_ids, comp)):
        result = runs(ids, table)
        dirty = sum(1 for value in result if value > 0)
        print("  %-4s 按住段 %-4d  段内夹零 %-4d (%.1f%%)"
              % (name, len(result), dirty, 100.0 * dirty / len(result) if result else 0.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
