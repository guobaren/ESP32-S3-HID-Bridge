#!/usr/bin/env python3
"""检查某个鼠标报告 body 在「到达(RX)」与「提交(SUBMIT)」两侧的出现位置。

用途：验证一份 buttons=0 的报告是否属于"先到达、后被延后提交"（后到先走），
还是根本不在 RX 序列里（合成路径产生）。

用法：
    python tools/probe_body_order.py <jsonl> 0000000004000000 [更多body...]
"""

from __future__ import annotations

import json
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, targets = sys.argv[1], sys.argv[2:]

    rx_hits: dict[str, list] = {t: [] for t in targets}
    sub_hits: dict[str, list] = {t: [] for t in targets}

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"diag_event"' not in line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            kind = record.get("kind_name")
            hexdata = record.get("data_hex") or ""
            event_id = record.get("event_id")
            ts = record.get("timestamp_us_low32")
            for target in targets:
                if kind == "RAW_HID_INPUT" and hexdata == "010208" + target:
                    rx_hits[target].append((event_id, ts))
                elif kind == "P_USB_REPORT_SUBMIT" and hexdata == "01010208" + target:
                    sub_hits[target].append((event_id, ts))

    for target in targets:
        rx = rx_hits[target]
        sub = sub_hits[target]
        print("body = %s" % target)
        print("  RX  出现 %d 次；前 10 个 (id, ts): %s" % (len(rx), rx[:10]))
        print("  SUB 出现 %d 次；前 10 个 (id, ts): %s" % (len(sub), sub[:10]))
        if rx and sub:
            first_rx = rx[0]
            first_sub = sub[0]
            print("  最早到达 id=%s，最早提交 id=%s → 提交%s到达"
                  % (first_rx[0], first_sub[0],
                     "晚于" if first_sub[0] > first_rx[0] else "不晚于"))
            # 该 body 每次提交时，RX 侧已经有多少更晚的报告先被提交？
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
