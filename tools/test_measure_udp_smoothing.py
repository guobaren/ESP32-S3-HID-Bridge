from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.measure_udp_smoothing import (
    Command,
    Sample,
    capture_cursor_sample,
    circle_metrics,
    judge_circle,
    send_command,
    write_command_csv,
)


class FakeClock:
    def __init__(self, now_ns: int = 0) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns

    def advance_us(self, amount: int) -> None:
        self.now_ns += amount * 1_000


class AdvancingSender:
    def __init__(self, clock: FakeClock, call_duration_us: int) -> None:
        self._clock = clock
        self._call_duration_us = call_duration_us

    def send(self, _dx: int, _dy: int, clock_ns) -> tuple[int, int]:
        started_ns = clock_ns()
        self._clock.advance_us(self._call_duration_us)
        return started_ns, clock_ns()


class MeasureUdpSmoothingTimingTests(unittest.TestCase):
    def test_cursor_sample_timestamp_is_taken_after_reader_returns(self) -> None:
        clock = FakeClock()

        def read_cursor() -> tuple[int, int]:
            clock.advance_us(1_500)
            return 12, 34

        sample = capture_cursor_sample(0, read_cursor, clock)

        self.assertEqual(sample, Sample(1_500, 12, 34))

    def test_send_timestamps_bracket_sender_call(self) -> None:
        clock = FakeClock(4_000_000)
        commands: list[Command] = []

        command = send_command(
            AdvancingSender(clock, 700),
            0,
            commands,
            1,
            -1,
            3_000,
            clock,
        )

        self.assertEqual(command.planned_us, 3_000)
        self.assertEqual(command.sent_us, 4_000)
        self.assertEqual(command.completed_us, 4_700)
        self.assertEqual(command.completed_us - command.sent_us, 700)
        self.assertIs(commands[0], command)

    def test_sample_gap_is_reported_alongside_no_motion_fail(self) -> None:
        samples = [
            Sample(192_000, 0, 0),
            Sample(198_000, 0, 0),
            Sample(361_052, 1, 0),
            Sample(363_052, 1, 0),
            Sample(526_000, 2, 0),
        ]
        commands = [Command(1, 191_000, 192_000, 192_010, 1, 0)]

        result = circle_metrics(
            samples,
            commands,
            2,
            0,
            (2, 0),
            (2, 0),
            (0, 0),
        )

        self.assertEqual(result["max_no_motion_gap_us"], 169_052)
        self.assertEqual(result["max_sample_interval_us"], 163_052)
        self.assertEqual(result["max_no_motion_gap_sample_interval_us"], 163_052)
        self.assertEqual(result["max_no_motion_gap_sample_count"], 2)
        result["commands_sent"] = 10_000
        failures = judge_circle(result)
        self.assertTrue(any("位移停顿" in failure and "169.1ms" in failure for failure in failures))
        self.assertTrue(any("不能单独定位输入链路" in failure for failure in failures))

    def test_command_csv_contains_plan_send_and_completion_timestamps(self) -> None:
        command = Command(1, 1_000, 1_250, 1_400, -1, 1)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "commands.csv"
            write_command_csv(path, [command])
            lines = path.read_text(encoding="utf-8").splitlines()

        self.assertEqual(
            lines[0],
            "index,planned_us,send_started_us,send_completed_us,send_call_duration_us,send_lag_us,dx,dy",
        )
        self.assertEqual(lines[1], "1,1000,1250,1400,150,250,-1,1")


if __name__ == "__main__":
    unittest.main()
