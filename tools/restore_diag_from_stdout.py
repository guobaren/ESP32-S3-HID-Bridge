#!/usr/bin/env python3
"""从 `dual_uart_inspect.py` 的 stdout 日志重建 DIAG_STREAM 的 JSONL 记录。

背景：`artifacts/tests/click-dup-20261014/` 下的 `.jsonl` 在 2026-10-14 17:17:35 被整体
重建（`CreationTime` 与 `LastWriteTime` 被同时重置），内容只剩开头数秒；原始 64 MB /
190 MB 的完整文件没有其他副本。但采集进程的 stdout 日志被完整保留，且每行都带有
`event_id / source / kind / len / hex`，足以重建事件流供既有分析工具使用。

stdout 行格式：
    [PC_DEVICE/COM13 2026-10-14T06:49:09.084+00:00 board_us=2161216672] event_id=549226 \
        source=UART1_RX kind=RAW_HID_INPUT len=11 hex=0102080000030004000000
    [PC_DEVICE/COM13] DIAG_STATUS captured=0 dropped=0

恢复边界（必须如实记录）：
  * 只恢复 stdout 已打印的字段；`.jsonl` 独有的 `underlying_payload_truncated`、
    `declared_payload_length`、`relative_ms` 等无法还原；
  * `timestamp_us_low32` 由 `board_us` 取低 32 位得到，与原始记录语义一致；
  * 非 ASCII 行（中文提示）在 stdout 中是乱码，一律跳过，不影响事件行解析。

用法：
    python tools/restore_diag_from_stdout.py <stdout.log> <输出.jsonl> [--summary 输出.json]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Optional

UINT32 = 0xFFFFFFFF

EVENT_RE = re.compile(
    r"^\[(?P<role>[A-Z_]+)/(?P<port>[A-Za-z0-9]+)\s+"
    r"(?P<observed>\S+)\s+board_us=(?P<board_us>\d+)\]\s+"
    r"event_id=(?P<event_id>\d+)\s+"
    r"source=(?P<source>\S+)\s+"
    r"kind=(?P<kind>\S+)\s+"
    r"len=(?P<length>\d+)\s+"
    r"hex=(?P<hex>[0-9a-fA-F]*)\s*$"
)
STATUS_RE = re.compile(
    r"^\[(?P<role>[A-Z_]+)/(?P<port>[A-Za-z0-9]+)\]\s+DIAG_STATUS\s+"
    r"captured=(?P<captured>\d+)\s+dropped=(?P<dropped>\d+)\s*$"
)


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="从 stdout 日志重建 DIAG_STREAM JSONL")
    parser.add_argument("stdout_log")
    parser.add_argument("output_jsonl")
    parser.add_argument("--summary", help="同时写出恢复摘要 JSON")
    args = parser.parse_args()

    events = 0
    statuses = []
    skipped = 0
    first: Optional[tuple] = None
    last: Optional[tuple] = None
    kinds: dict = {}
    role = ""
    port = ""

    source_size = os.path.getsize(args.stdout_log)
    with open(args.stdout_log, "r", encoding="utf-8", errors="replace") as source, \
            open(args.output_jsonl, "w", encoding="utf-8", newline="\n") as target:
        for raw_line in source:
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            match = EVENT_RE.match(line)
            if match:
                role = match.group("role")
                port = match.group("port")
                event_id = int(match.group("event_id"))
                board_us = int(match.group("board_us"))
                kind = match.group("kind")
                data_hex = match.group("hex")
                record = {
                    "observed_at_utc": match.group("observed"),
                    "port": port,
                    "role": role,
                    "board_us": board_us,
                    "record_type": "diag_event",
                    "event_id": event_id,
                    "timestamp_us_low32": board_us & UINT32,
                    "source_name": match.group("source"),
                    "kind_name": kind,
                    "data_length": int(match.group("length")),
                    "data_hex": data_hex,
                    "recovered_from": "stdout_log",
                }
                target.write(json.dumps(record, ensure_ascii=False) + "\n")
                events += 1
                kinds[kind] = kinds.get(kind, 0) + 1
                if first is None:
                    first = (event_id, board_us, match.group("observed"))
                last = (event_id, board_us, match.group("observed"))
                continue
            match = STATUS_RE.match(line)
            if match:
                role = match.group("role")
                port = match.group("port")
                captured = int(match.group("captured"))
                dropped = int(match.group("dropped"))
                statuses.append({"captured": captured, "dropped": dropped,
                                 "observed_at_utc": ""})
                target.write(json.dumps({
                    "port": port, "role": role,
                    "record_type": "diag_stream_status",
                    "captured": captured, "dropped": dropped,
                    "recovered_from": "stdout_log",
                }, ensure_ascii=False) + "\n")
                continue
            skipped += 1

    summary = {
        "source_stdout_log": os.path.abspath(args.stdout_log),
        "source_bytes": source_size,
        "output_jsonl": os.path.abspath(args.output_jsonl),
        "role": role,
        "port": port,
        "diag_events": events,
        "skipped_lines": skipped,
        "kind_counts": kinds,
        "event_id_first": first[0] if first else None,
        "event_id_last": last[0] if last else None,
        "board_us_first": first[1] if first else None,
        "board_us_last": last[1] if last else None,
        "wall_first": first[2] if first else None,
        "wall_last": last[2] if last else None,
        "span_seconds": ((last[1] - first[1]) & UINT32) / 1e6 if first and last else None,
        "status_first": statuses[0] if statuses else None,
        "status_last": statuses[-1] if statuses else None,
        "recovery_note": ("由采集进程 stdout 日志重建；不含 .jsonl 独有的 "
                          "underlying_payload_truncated / declared_payload_length / relative_ms 字段"),
    }
    if statuses:
        gained = statuses[-1]["captured"] - statuses[0]["captured"]
        lost = statuses[-1]["dropped"] - statuses[0]["dropped"]
        produced = gained + lost
        summary["captured_gain"] = gained
        summary["dropped_gain"] = lost
        summary["produced"] = produced
        summary["drop_ratio"] = (lost / produced) if produced else None

    if args.summary:
        with open(args.summary, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, ensure_ascii=False, indent=2)

    print("恢复完成")
    print("  来源 stdout : %s (%d 字节)" % (args.stdout_log, source_size))
    print("  输出 JSONL  : %s (%d 字节)" % (args.output_jsonl,
                                            os.path.getsize(args.output_jsonl)))
    print("  diag_event  : %d 条，跳过非事件行 %d 行" % (events, skipped))
    print("  diag_status : %d 条" % len(statuses))
    if first and last:
        print("  event_id    : %d .. %d" % (first[0], last[0]))
        print("  板内时窗    : %d .. %d（跨度 %.3f s）"
              % (first[1], last[1], ((last[1] - first[1]) & UINT32) / 1e6))
        print("  墙钟时窗    : %s .. %s" % (first[2], last[2]))
    if statuses:
        print("  captured 增量: %d, dropped 增量: %d, 复算丢弃率: %s"
              % (summary.get("captured_gain", 0), summary.get("dropped_gain", 0),
                 ("%.2f%%" % (summary["drop_ratio"] * 100)) if summary.get("drop_ratio") else "无法计算"))
    for kind, count in sorted(kinds.items(), key=lambda item: -item[1]):
        print("    %-24s %d" % (kind, count))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
