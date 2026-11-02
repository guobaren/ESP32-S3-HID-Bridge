from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from read_device_stats import COUNTER_NAMES, SnapshotCollector, STATS_RESPONSE  # noqa: E402
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

    if count != 34 or set(collectors) != {1, 2}:
        raise AssertionError(f"expected 34 P/M pages, got {count} pages and roles {sorted(collectors)}")
    pc = collectors[1].result()
    mouse = collectors[2].result()
    if pc["role"] != "P/PC_DEVICE" or mouse["role"] != "M/MOUSE_HOST":
        raise AssertionError("role mapping must be P=1 and M=2")
    if mouse["counters"]["motion_rx_dx"] != -12345 or mouse["counters"]["motion_rx_dy"] != -6789:
        raise AssertionError("signed M motion counters decoded incorrectly")
    if pc["state"]["lastResult"] != "mounted_acked" or not pc["state"]["mounted"] or \
            not mouse["state"]["firstVendorRequestSeen"] or mouse["state"]["vendorSessionEpoch"] != 33:
        raise AssertionError("P/M USB and vendor session state bitfields decoded incorrectly")
    host_diag = mouse["state"]["usbHostDiagnostics"]
    if not host_diag["available"] or host_diag["backend"] != "CherryUSB" or \
            host_diag["schemaVersion"] != 1 or not host_diag["registerSnapshotValid"] or \
            not host_diag["connected"] or host_diag["failureObservationCount"] != 1:
        raise AssertionError("USB Host diagnostics capability, live-state flags, or failure count mismatch")
    if host_diag["lastRawError"] != -7 or host_diag["lastFailureRawError"] != -9 or \
            host_diag["lastStage"] != "enumerate_success" or \
            host_diag["lastFailureStage"] != "hid_init_urb_raw" or \
            host_diag["lastConnectedPcDetMs"] != 9600:
        raise AssertionError("USB Host diagnostics signed errors, retained failure, stage, or PCDET wake time mismatch")
    if pc["state"]["usbHostDiagnostics"]["available"]:
        raise AssertionError("P board must report CherryUSB diagnostics as unsupported")
    if len(mouse["counters"]) != 83:
        raise AssertionError(f"M counters must cross 64-item boundary and contain 83 fields, got {len(mouse['counters'])}")
    ring = next(queue for queue in mouse["queues"] if queue["id"] == 13)
    if ring["unit"] != "bytes" or ring["capacity"] != 16384 or ring["depth"] != 23:
        raise AssertionError("UART1 RX ring unit/capacity/depth mismatch")
    raw_input = next(queue for queue in pc["queues"] if queue["id"] == 10)
    retired_motion_input = next(queue for queue in pc["queues"] if queue["id"] == 11)
    if raw_input["unit"] != "items" or raw_input["capacity"] != 128 or \
            raw_input["received"] != 225 or raw_input["dropped"] != 3:
        raise AssertionError("P unified raw-input queue capacity or metrics mismatch")
    if any(retired_motion_input[key] != 0 for key in (
            "capacity", "depth", "peak", "received", "rejected", "dropped")):
        raise AssertionError("retired P motion-input Queue ID must remain an explicit zero slot")
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
    collector = collectors[2]
    missing_diag = collector.counters.pop("usb_host_last_connected_pcdet_ms")
    try:
        collector.result()
    except ValueError as error:
        if "96" not in str(error):
            raise AssertionError("缺少诊断字段96时应报告确切缺项") from error
    else:
        raise AssertionError("缺少诊断字段96时仍被接受")
    finally:
        collector.counters["usb_host_last_connected_pcdet_ms"] = missing_diag

    def clone_mouse_collector() -> SnapshotCollector:
        clone = SnapshotCollector()
        clone.role = collector.role
        clone.uptime_ms = collector.uptime_ms
        clone.page_counts = dict(collector.page_counts)
        clone.pages = {kind: set(indices) for kind, indices in collector.pages.items()}
        clone.counters = dict(collector.counters)
        clone.counter_ids = set(collector.counter_ids)
        clone.queues = {key: dict(value) for key, value in collector.queues.items()}
        return clone

    legacy_m = clone_mouse_collector()
    legacy_m.counter_ids = {item_id for item_id in legacy_m.counter_ids
                            if not 69 <= item_id <= 96}
    legacy_m.counters = {name: value for name, value in legacy_m.counters.items()
                         if not name.startswith("usb_host_")}
    if legacy_m.result()["state"]["usbHostDiagnostics"]["available"]:
        raise AssertionError("旧 M 固件缺少 ID69 时必须显式标记 diagnostics unsupported")

    invalid_flags_m = clone_mouse_collector()
    invalid_flags_m.counters[COUNTER_NAMES[69]] = 0
    try:
        invalid_flags_m.result()
    except ValueError as error:
        if "schema" not in str(error):
            raise AssertionError("diagnostics flags/schema无效时应明确拒绝") from error
    else:
        raise AssertionError("diagnostics flags 未声明 supported 时仍被当作可用")

    no_event_m = clone_mouse_collector()
    flags_name = COUNTER_NAMES[69]
    no_event_m.counters[flags_name] &= ~((1 << 11) | (1 << 12) | (1 << 13))
    for item_id in (74, 75, 76, 80, 83, 84, 85, 92):
        no_event_m.counters[COUNTER_NAMES[item_id]] = 0
    no_event_diag = no_event_m.result()["state"]["usbHostDiagnostics"]
    if no_event_diag["lastStage"] is not None or \
            no_event_diag["lastConnectedPcDetMs"] is not None or \
            no_event_diag["hprt"] is not None or \
            no_event_diag["connected"] is not None or \
            no_event_diag["inputAgeMs"] is not None:
        raise AssertionError("未观察到事件/快照无效时，最新阶段、wake时间、寄存器与生命周期必须为 unknown")
    zero_time_m = clone_mouse_collector()
    zero_time_m.counters[COUNTER_NAMES[80]] = 1
    zero_time_m.counters[COUNTER_NAMES[83]] = 0
    zero_time_m.counters[COUNTER_NAMES[85]] = 0
    zero_time_m.counters[COUNTER_NAMES[86]] = 2  # debounce
    zero_time_m.counters[COUNTER_NAMES[87]] = -(1 << 31)
    zero_time_m.counters[COUNTER_NAMES[88]] = 0
    zero_time_diag = zero_time_m.result()["state"]["usbHostDiagnostics"]
    if zero_time_diag["lastStage"] != "debounce" or \
            zero_time_diag["lastStageMs"] != 0 or \
            zero_time_diag["lastRawError"] is not None:
        raise AssertionError("事件计数必须使 time=0 的最新阶段有效，INT32_MIN 哨兵必须解码为 null")
    print("Python stats fixture: PASS; 34 pages/83 counters, unsupported and missing-field rejection, no-event unknowns, time=0 stage, INT32_MIN=>null, retained failure, and 20-byte queues.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
