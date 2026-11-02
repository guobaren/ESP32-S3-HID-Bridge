#!/usr/bin/env python3
"""输出一次 DIAG_STREAM 采集的窗口档案，使每轮丢弃率都能被独立复算。

记录三类信息：
  1. `diag_event` 的 event_id 范围与板内/墙钟时窗（确认采集是否完整覆盖目标区间）；
  2. `diag_stream_status` 的**首条与末条** captured/dropped（起止状态，用于做差）；
  3. 由 (末 - 首) 复算本轮的真实丢弃率。

背景：`captured`/`dropped` 是板子开机以来的累计值
（`firmware/main/diag_stream.c:130-137`：队列已存在时 `dual_diag_stream_start` 直接返回、
不重置计数），因此**必须用同一次采集内的首末状态做差**，不能直接引用终值。

用法：
    python tools/diag_capture_report.py <jsonl> [--require-id 375606-375620] [--out report.txt]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import List, Optional, Tuple

UINT32 = 0xFFFFFFFF


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="DIAG_STREAM 采集窗口档案与丢弃率复算")
    parser.add_argument("jsonl")
    parser.add_argument("--require-id", help="校验是否覆盖该 event_id 区间，格式 A-B")
    parser.add_argument("--out")
    args = parser.parse_args()

    lines: List[str] = []
    handle = open(args.out, "w", encoding="utf-8") if args.out else None

    def out(text: str = "") -> None:
        lines.append(text)
        if handle is not None:
            handle.write(text + "\n")

    first_event: Optional[Tuple[int, int, str]] = None  # id, ts, observed
    last_event: Optional[Tuple[int, int, str]] = None
    event_count = 0
    statuses: List[Tuple[int, int, str, str]] = []  # captured, dropped, observed, seq
    id_set = set()
    truncated = 0

    with open(args.jsonl, "r", encoding="utf-8", errors="replace") as source:
        for raw_line in source:
            line = raw_line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            kind = record.get("record_type")
            if kind == "diag_event":
                event_count += 1
                event_id = int(record.get("event_id", 0))
                ts = int(record.get("timestamp_us_low32", 0))
                observed = record.get("observed_at_utc", "")
                if record.get("underlying_payload_truncated"):
                    truncated += 1
                if first_event is None:
                    first_event = (event_id, ts, observed)
                last_event = (event_id, ts, observed)
                if args.require_id:
                    id_set.add(event_id)
            elif kind == "diag_stream_status":
                statuses.append((int(record.get("captured", 0)),
                                 int(record.get("dropped", 0)),
                                 record.get("observed_at_utc", ""),
                                 str(record.get("sequence", ""))))

    size = os.path.getsize(args.jsonl)
    out("文件            : %s" % args.jsonl)
    out("文件大小        : %d 字节 (%.1f MB)" % (size, size / 1048576.0))
    out("diag_event 条数 : %d" % event_count)
    out("载荷被截断条数  : %d" % truncated)

    if first_event and last_event:
        span_us = (last_event[1] - first_event[1]) & UINT32
        out("event_id 范围   : %d .. %d（跨度 %d）"
            % (first_event[0], last_event[0], last_event[0] - first_event[0]))
        out("板内时窗        : %d .. %d（跨度 %.3f s）"
            % (first_event[1], last_event[1], span_us / 1e6))
        out("墙钟时窗        : %s .. %s" % (first_event[2], last_event[2]))

    out()
    out("diag_stream_status 记录数: %d" % len(statuses))
    if statuses:
        first_status = statuses[0]
        last_status = statuses[-1]
        out("  首条: captured=%d dropped=%d  seq=%s  %s"
            % (first_status[0], first_status[1], first_status[3], first_status[2]))
        out("  末条: captured=%d dropped=%d  seq=%s  %s"
            % (last_status[0], last_status[1], last_status[3], last_status[2]))
        gained = last_status[0] - first_status[0]
        lost = last_status[1] - first_status[1]
        produced = gained + lost
        out("  本轮增量: captured += %d, dropped += %d, 产生 = %d" % (gained, lost, produced))
        if produced > 0:
            out("  复算丢弃率: %d / %d = %.2f%%" % (lost, produced, 100.0 * lost / produced))
        else:
            out("  复算丢弃率: 无法计算（产生量为 0，可能未真正订阅）")
        # 单调性自检：累计值必须非递减
        regressions = sum(1 for i in range(1, len(statuses))
                          if statuses[i][0] < statuses[i - 1][0]
                          or statuses[i][1] < statuses[i - 1][1])
        out("  累计值回退次数: %d %s" % (regressions,
                                          "（正常，累计值非递减）" if regressions == 0
                                          else "（异常，需检查是否复位过）"))

    if args.require_id:
        try:
            low_text, high_text = args.require_id.split("-")
            low, high = int(low_text), int(high_text)
        except ValueError:
            out()
            out("--require-id 格式无效，应为 A-B")
        else:
            wanted = set(range(low, high + 1))
            missing = sorted(wanted - id_set)
            out()
            out("event_id 区间校验 %d..%d：覆盖 %d/%d"
                % (low, high, len(wanted) - len(missing), len(wanted)))
            if missing:
                preview = ", ".join(str(item) for item in missing[:40])
                out("  缺失: %s%s" % (preview, " ..." if len(missing) > 40 else ""))
            else:
                out("  该区间在本次采集中完整存在")

    if handle is not None:
        handle.close()
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
