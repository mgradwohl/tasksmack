#!/usr/bin/env python3
"""Tests for tools/resize-perf-summary.awk, measure-idle.sh's ResizePerf summary reader (#3210).

A summary of a window that drew no frame (frames=0 loopIntervals=0, a minimized window) is left out of
the frame and loop figures: before #3210 the app printed the previous drawn window's p95/p99 on it, so
measure-idle reported a loop p95 for wakeups that never happened. With no summary that drew a frame the
figures print as n/a. Canned log lines through the awk program, run by bash's awk; nothing is launched.
"""

import os
import shutil
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AWK_PROGRAM = REPO_ROOT / "tools" / "resize-perf-summary.awk"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"


def summary(frames, frame, loops, loop, reason="idle-progress"):
    """One summary line as Core::logResizePerfTraceSummary logs it; frame and loop are avg/p95/p99/max."""
    zero = "0.000/0.000/0.000/0.000"
    return (
        f"[2026-10-28 01:39:39.578] [info] ResizePerf[{reason}]: batches=5 events=5 resizeEvents=0 inputEvents=0 "
        f"maxBatchEvents=1 p0Hits=0 skippedFrames=0 maxSingleEvent=0.010 ms frames={frames} resizeFrames=0 "
        f"frame avg/p95/p99/max={frame} ms over100=0 over250=0 loopIntervals={loops} loop avg/p95/p99/max={loop} ms "
        f"drain avg/p95/p99/max={zero} ms update avg/p95/p99/max={zero} ms render avg/p95/p99/max={zero} ms "
        f"post avg/p95/p99/max={zero} ms swap avg/p95/p99/max={zero} ms"
    )


DRAWN = summary(40, "1.000/1.500/2.000/2.500", 40, "63.113/64.394/64.721/64.721")
# What a pre-#3210 build logged for the next, empty window: stale p95/p99 beside a zero avg and max.
STALE_EMPTY = summary(0, "0.000/9.000/9.500/0.000", 0, "0.000/64.394/64.721/0.000")
EMPTY = summary(0, "0.000/0.000/0.000/0.000", 0, "0.000/0.000/0.000/0.000")


class ResizePerfSummaryAwkTest(unittest.TestCase):
    def read(self, *lines):
        """The awk program's one output line for these log lines, split into its fields."""
        text = "".join(f"{line}\n" for line in lines)
        result = subprocess.run(
            [BASH, "-c", 'awk -f "$1"', "bash", AWK_PROGRAM.as_posix()],
            input=text,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.split()

    def test_drawn_windows_report_their_worst_figures(self):
        faster = summary(100, "0.500/1.000/3.000/4.000", 100, "20.000/25.000/30.000/40.000")
        # fps: 140 intervals over 40 * 63.113 + 100 * 20 = 4524.52 ms.
        self.assertEqual(self.read(DRAWN, faster), ["2", "30.94", "1.500", "3.000", "4.000", "64.394", "64.721"])

    def test_an_empty_window_is_left_out_of_the_frame_and_loop_figures(self):
        smaller = summary(10, "0.100/0.200/0.300/0.400", 10, "10.000/11.000/12.000/13.000")
        self.assertEqual(
            self.read(smaller, STALE_EMPTY, EMPTY),
            ["3", "100.00", "0.200", "0.300", "0.400", "11.000", "12.000"],
        )

    def test_only_empty_windows_report_n_a(self):
        # A minimized window: the summaries are counted, but they have no frame or loop figures.
        self.assertEqual(self.read(STALE_EMPTY, EMPTY), ["2", "0.00", "n/a", "n/a", "n/a", "n/a", "n/a"])

    def test_a_frame_without_a_loop_interval_keeps_its_frame_figures(self):
        # The first frame after a full reset has work but no deliver-to-deliver interval yet.
        first = summary(1, "5.000/5.000/5.000/5.000", 0, "0.000/0.000/0.000/0.000", reason="interaction-end")
        self.assertEqual(self.read(first), ["1", "0.00", "5.000", "5.000", "5.000", "n/a", "n/a"])

    def test_no_summaries_report_n_a(self):
        self.assertEqual(
            self.read("[2026-10-28 01:39:39.578] [info] Entering main loop"),
            ["0", "0.00", "n/a", "n/a", "n/a", "n/a", "n/a"],
        )


if __name__ == "__main__":
    unittest.main()
