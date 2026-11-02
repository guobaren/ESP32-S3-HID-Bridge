#!/usr/bin/env python3
"""按时间窗口剖析 DIAG_STREAM 的丢弃分布与事件类型构成。

原理：`event_id` 由 `firmware/main/diag_stream.c:192` 的全局原子计数器分配、单调递增，
因此同一窗口内 (id_max - id_min + 1) 就是该时段"产生"的事件总数，减去实际记录数即为
被丢弃数。由此可算出每个时间窗口的丢弃率，并看到各类事件在同一窗口的记录构成。

注意：`captured` / `dropped`（DIAG_STREAM_STATUS）是**开机以来的累计值**，
`firmware/main/diag_stream.c:130-137` 在队列已存在时直接返回、不重置计数。
跨轮比较必须做差，不能直接引用绝对值。

用法：
    python tools/diag_drop_profile.py <jsonl> [--bucket-s 10]
"""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict

UINT32 = 0xFFFFFFFF
KINDS = ("RAW_HID_INPUT", "P_USB_REPORT_SUBMIT", "P_USB_REPORT_COMPLETE")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("jsonl")
    parser.add_argument("--bucket-s", type=float, default=10.0)
    parser.add_argument("--top", type=int, default=0,
                        help="只列出丢弃率最高的 N 个窗口")
    args = parser.parse_args()

    rows = []
    with open(args.jsonl, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"kind_name"' not in line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("record_type") != "diag_event":
                continue
            rows.append((int(record.get("timestamp_us_low32", 0)),
                         int(record.get("event_id", 0)),
                         record.get("kind_name") or "?"))
    if not rows:
        print("无事件")
        return 0

    base = rows[0][0]
    bucket_us = int(args.bucket_s * 1_000_000)
    buckets = defaultdict(lambda: {"count": 0, "id_min": None, "id_max": None,
                                   "kinds": Counter()})
    for ts, event_id, kind in rows:
        key = ((ts - base) & UINT32) // bucket_us
        bucket = buckets[key]
        bucket["count"] += 1
        bucket["kinds"][kind] += 1
        if bucket["id_min"] is None or event_id < bucket["id_min"]:
            bucket["id_min"] = event_id
        if bucket["id_max"] is None or event_id > bucket["id_max"]:
            bucket["id_max"] = event_id

    summary = []
    for key in sorted(buckets):
        bucket = buckets[key]
        produced = bucket["id_max"] - bucket["id_min"] + 1
        lost = produced - bucket["count"]
        summary.append((key, bucket, produced, lost))

    print("文件：%s" % args.jsonl)
    print("事件总数（已记录）= %d，窗口 = %.0f s，共 %d 个窗口"
          % (len(rows), args.bucket_s, len(summary)))
    print()
    print(" 窗口起点s   记录数    产生数    丢弃数   丢弃率    RX    SUB   COMP   其它")
    shown = summary
    if args.top:
        shown = sorted(summary, key=lambda item: item[3], reverse=True)[:args.top]
        shown.sort()
    for key, bucket, produced, lost in shown:
        kinds = bucket["kinds"]
        other = bucket["count"] - sum(kinds.get(k, 0) for k in KINDS)
        print("  %8.0f  %8d  %8d  %8d  %6.1f%%  %5d  %5d  %5d  %5d"
              % (key * args.bucket_s, bucket["count"], produced, lost,
                 100.0 * lost / produced if produced else 0.0,
                 kinds.get("RAW_HID_INPUT", 0), kinds.get("P_USB_REPORT_SUBMIT", 0),
                 kinds.get("P_USB_REPORT_COMPLETE", 0), other))

    total_rec = sum(item[1]["count"] for item in summary)
    total_prod = sum(item[2] for item in summary)
    print()
    print("合计：记录 %d，产生 %d，丢弃 %d（%.1f%%）"
          % (total_rec, total_prod, total_prod - total_rec,
             100.0 * (total_prod - total_rec) / total_prod if total_prod else 0.0))

    per_kind = Counter()
    for item in summary:
        per_kind.update(item[1]["kinds"])
    print()
    print("按类型记录数（三层应接近 1:1:1，偏离说明该类丢得更多）:")
    for kind, count in per_kind.most_common(8):
        print("  %-24s %d" % (kind, count))

    busy = sorted(summary, key=lambda item: item[2], reverse=True)[:5]
    print()
    print("产生量最高的 5 个窗口（负载最高）:")
    for key, bucket, produced, lost in busy:
        print("  %6.0f s: 产生 %d，记录 %d，丢弃率 %.1f%%"
              % (key * args.bucket_s, produced, bucket["count"],
                 100.0 * lost / produced if produced else 0.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
