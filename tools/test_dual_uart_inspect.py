from __future__ import annotations

import struct
import contextlib
import io
import json
import tempfile
import time
import unittest
from pathlib import Path

from tools.dual_uart_inspect import (
    DiagEventReassembler,
    Frame,
    MixedStreamParser,
    PortSession,
    PROFILE_HEADER_SIZE,
    PROFILE_MAGIC,
    TYPE_DEVICE_HELLO,
    build_frame,
    diag_event_details,
    parse_device_hello,
    parse_log_line,
    parse_profile_blob,
    print_status,
    mouse_move_payload,
)
from tools.dual_uart_inspect import TYPE_DIAG_STREAM_EVENT, TYPE_DIAG_STREAM_STATUS


def diag_fragment(event_id: int, timestamp: int, source: int, kind: int,
                  data: bytes, offset: int) -> bytes:
    chunk = data[offset:offset + 52]
    return struct.pack("<IIBBBB", event_id, timestamp, source, kind,
                       len(data), offset) + chunk


class DualUartInspectTests(unittest.TestCase):
    def test_move_probe_uses_existing_seven_byte_mouse_report(self) -> None:
        self.assertEqual(mouse_move_payload(20, -3), struct.pack("<Bhhbb", 0, 20, -3, 0, 0))
        with self.assertRaises(ValueError):
            mouse_move_payload(0, 0)
        with self.assertRaises(ValueError):
            mouse_move_payload(32768, 0)
    def test_mixed_text_and_split_frame(self) -> None:
        lines: list[tuple[bytes, bool, bool]] = []
        parser = MixedStreamParser(lambda raw, terminated, truncated:
                                   lines.append((raw, terminated, truncated)))
        wire = build_frame(0x07, 9, b"HIDBRDG2" + b"12345678" + b"\x02")
        self.assertEqual(parser.feed(b"boot line\r\n" + wire[:4]), [])
        frames = parser.feed(wire[4:] + b"UART1 stats\n")
        self.assertEqual(len(frames), 1)
        self.assertEqual((frames[0].message_type, frames[0].sequence), (0x07, 9))
        self.assertEqual([entry[0] for entry in lines], [b"boot line", b"UART1 stats"])
        self.assertTrue(all(entry[1] for entry in lines))

    def test_bad_crc_resynchronizes_to_following_frame(self) -> None:
        parser = MixedStreamParser(lambda *_args: None)
        bad = bytearray(build_frame(0x07, 1, b"bad"))
        bad[-1] ^= 0x80
        good = build_frame(0x07, 2, b"good")
        frames = parser.feed(bytes(bad) + good)
        self.assertEqual(parser.invalid_candidates, 1)
        self.assertEqual([frame.sequence for frame in frames], [2])

    def test_device_hello_requires_matching_nonce(self) -> None:
        nonce = b"n0nce123"
        frame = Frame(2, TYPE_DEVICE_HELLO, 4, b"HIDBRDG2" + nonce + b"\x01", b"")
        self.assertEqual(parse_device_hello(frame, {nonce}), 1)
        self.assertIsNone(parse_device_hello(frame, {b"other123"}))

    def test_profile_summary_and_console_statistics(self) -> None:
        device = bytearray(18)
        device[0:2] = b"\x12\x01"
        struct.pack_into("<HH", device, 8, 0x046D, 0xC092)
        blob = struct.pack("<IHHHHHHHBB", PROFILE_MAGIC, 2, PROFILE_HEADER_SIZE,
                           len(device), 0, 0, 0, 0, 1, 0)
        blob += bytes(device) + struct.pack("<BBBBH", 0, 1, 2, 0, 3) + b"\x05\x01\x09"
        summary = parse_profile_blob(blob)
        self.assertEqual(summary["vid_pid"], "046D:C092")
        self.assertEqual(summary["report_descriptor_count"], 1)

        line = parse_log_line(
            "UART1统计 tx=9 rx=8 drop=2 vendor_drop=1 peer=online",
            "uart0_live", "2026-09-27T00:00:00+00:00", 1.0)
        self.assertEqual(line["fields"]["drop"], "2")
        self.assertEqual(line["fields"]["peer"], "online")

    def test_queue_stats_console_lines_are_retained_and_displayed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            session = PortSession("COM-test", Path(temporary), time.monotonic())
            observed_at = "2026-09-27T00:00:00+00:00"
            queue_line = (
                "I (12345) dual_hid_host: QUEUE name=host_hid_report "
                "received=120 rejected=2 dropped=3 peak=17")
            session._update_status(
                queue_line,
                parse_log_line(queue_line, "uart0_live", observed_at, 1.0),
                time.monotonic())
            event_line = (
                "I (12346) dual_uart1: QUEUE name=uart1_event "
                "consumed=80 overflow=1 dropped=4 peak=128")
            session._update_status(
                event_line,
                parse_log_line(event_line, "uart0_live", observed_at, 2.0),
                time.monotonic())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                print_status([session], "live")
            session.close()

            self.assertEqual(session.status.queue_metrics["host_hid_report"]["received"], 120)
            self.assertEqual(session.status.queue_metrics["host_hid_report"]["peak"], 17)
            self.assertEqual(session.status.queue_metrics["uart1_event"]["consumed"], 80)
            self.assertIn("host_hid_report=rx:120,rej:2,drop:3,peak:17", output.getvalue())
            self.assertIn("uart1_event=consumed:80,overflow:1,drop:4,peak:128",
                          output.getvalue())

    def test_diag_event_reassembles_out_of_order_64_byte_payload(self) -> None:
        reassembler = DiagEventReassembler()
        raw = bytes(range(64))
        event, errors = reassembler.feed(
            diag_fragment(0x10203040, 0x89ABCDEF, 4, 0x02, raw, 52))
        self.assertIsNone(event)
        self.assertEqual(errors, [])
        event, errors = reassembler.feed(
            diag_fragment(0x10203040, 0x89ABCDEF, 4, 0x02, raw, 0))
        self.assertEqual(errors, [])
        self.assertIsNotNone(event)
        self.assertEqual(event["data"], raw)
        self.assertEqual(event["fragment_count"], 2)
        self.assertEqual(reassembler.completed, 1)

    def test_diag_event_detects_conflict_and_incomplete_tail(self) -> None:
        reassembler = DiagEventReassembler()
        first = struct.pack("<IIBBBB", 11, 22, 3, 0x28, 12, 0) + b"abcdefgh"
        conflict = struct.pack("<IIBBBB", 11, 22, 3, 0x28, 12, 4) + b"ZZ"
        self.assertEqual(reassembler.feed(first), (None, []))
        event, errors = reassembler.feed(conflict)
        self.assertIsNone(event)
        self.assertTrue(any("overlapping fragment conflict" in item for item in errors))
        self.assertEqual(reassembler.finish(), [])

        incomplete = struct.pack("<IIBBBB", 12, 23, 3, 0x28, 12, 0) + b"abc"
        self.assertEqual(reassembler.feed(incomplete), (None, []))
        self.assertEqual(reassembler.finish(), [
            {"event_id": 12, "received_bytes": 3, "total_length": 12}
        ])

    def test_diag_event_marks_usb_payload_truncation_and_vendor_stall(self) -> None:
        submit = bytes((1, 0, 1, 64)) + bytes(range(60))
        details = diag_event_details(0x85, submit)
        self.assertTrue(details["underlying_payload_truncated"])
        self.assertEqual(details["declared_payload_length"], 64)
        self.assertEqual(details["captured_payload_length"], 60)

        setup = bytes((0, 0xC0, 0x22)) + struct.pack("<HHH", 0x0102, 3, 4) + b"\x00"
        details = diag_event_details(0x83, setup)
        self.assertEqual(details["callback_result"], "STALL (return false)")
        self.assertEqual(details["setup"]["wLength"], 4)

    def test_port_session_prints_events_and_reports_board_drops(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            session = PortSession("COM-test", Path(temporary), time.monotonic())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                payload = diag_fragment(7, 123456, 3, 0x28, b"\x01\x02\x03", 0)
                session._frame_record(Frame(2, TYPE_DIAG_STREAM_EVENT, 1,
                                             payload, b""))
                session._frame_record(Frame(2, TYPE_DIAG_STREAM_STATUS, 2,
                                             struct.pack("<II", 20, 2), b""))
            session.close()
            visible = output.getvalue()
            self.assertIn("event_id=7 source=UART1_RX kind=HID_SET_REPORT len=3", visible)
            self.assertIn("hex=010203", visible)
            self.assertIn("DIAG_STATUS captured=20 dropped=2", visible)
            records = [json.loads(line) for line in
                       (Path(temporary) / "COM-test.jsonl").read_text(encoding="utf-8").splitlines()]
            event_records = [item for item in records if item["record_type"] == "diag_event"]
            status_records = [item for item in records
                              if item["record_type"] == "diag_stream_status"]
            self.assertEqual(event_records[0]["data_hex"], "010203")
            self.assertEqual(status_records[0]["dropped"], 2)


if __name__ == "__main__":
    unittest.main()
