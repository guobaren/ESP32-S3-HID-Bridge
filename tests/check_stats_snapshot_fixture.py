from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from read_device_stats import SnapshotCollector, STATS_RESPONSE  # noqa: E402
from uart_protocol import FrameScanner  # noqa: E402


def main() -> int:
    fixture = ROOT / "tests" / "fixtures" / "stats_snapshot_c_frames.txt"
    collectors: dict[int, SnapshotCollector] = {}
    scanner = FrameScanner()
    count = 0
    for line in fixture.read_text(encoding="ascii").splitlines():
        role_text, _kind_text, _index_text, _pages_text, frame_hex = line.split(",", 4)
        role = int(role_text)
        frames = scanner.feed(bytes.fromhex(frame_hex))
        if len(frames) != 1:
            raise AssertionError(f"C fixture frame should decode exactly once: role={role}")
        message_type, sequence, payload = frames[0]
        if message_type != STATS_RESPONSE or sequence != 0xA17C:
            raise AssertionError(f"C fixture type/sequence mismatch: role={role}")
        collectors.setdefault(role, SnapshotCollector()).add(payload)
        count += 1

    if count != 28 or set(collectors) != {1, 2}:
        raise AssertionError(f"expected 28 P/M pages, got {count} pages and roles {sorted(collectors)}")
    pc = collectors[1].result()
    mouse = collectors[2].result()
    if pc["role"] != "P/PC_DEVICE" or mouse["role"] != "M/MOUSE_HOST":
        raise AssertionError("role mapping must be P=1 and M=2")
    if mouse["counters"]["motion_rx_dx"] != -12345 or mouse["counters"]["motion_rx_dy"] != -6789:
        raise AssertionError("signed M motion counters decoded incorrectly")
    if pc["state"]["lastResult"] != "mounted_acked" or not pc["state"]["mounted"] or \
            not mouse["state"]["firstVendorRequestSeen"] or mouse["state"]["vendorSessionEpoch"] != 33:
        raise AssertionError("P/M USB and vendor session state bitfields decoded incorrectly")
    ring = next(queue for queue in mouse["queues"] if queue["id"] == 13)
    if ring["unit"] != "bytes" or ring["capacity"] != 16384 or ring["depth"] != 23:
        raise AssertionError("UART1 RX ring unit/capacity/depth mismatch")
    event_queue = next(queue for queue in pc["queues"] if queue["id"] == 9)
    if event_queue["received"] is not None or event_queue["rejected"] is not None or event_queue["dropped"] != 17:
        raise AssertionError("event queue unknown counters/reset_dropped semantics mismatch")
    for role, state_name, state_id in ((1, "p_usb_state", 67), (2, "m_vendor_session_state", 68)):
        collector = collectors[role]
        state_value = collector.counters.pop(state_name)
        try:
            collector.result()
        except ValueError as error:
            if f"id=[{state_id}]" not in str(error):
                raise AssertionError(f"缺少必需状态 id={state_id} 时应报告缺失字段") from error
        else:
            raise AssertionError(f"缺少必需状态 id={state_id} 时仍被接受")
        finally:
            collector.counters[state_name] = state_value
    print("Python stats fixture 检查：PASS；28 个双板固件 C 编码 P/M 页具备必需状态，缺少 67/68 时拒绝，role、signed counter、20B queue 与 sentinel 匹配。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
