#!/usr/bin/env python3
"""Tests for tools/idle-sample.py, measure-idle.sh's per-phase /proc sampler (#3729).

The parsers are fed canned /proc text, the phase figures come from hand-made snapshots, and the
phase command runs against a fake /proc tree, so nothing is launched and no real process is read.
The field names are measure-idle.ps1's (schema tasksmack-idle-windows/2, #3715), so the tests pin
them too: a summary script reads both platforms' phases by those names.
"""

import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "idle-sample.py"
spec = importlib.util.spec_from_file_location("idle_sample", SCRIPT)
idle_sample = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idle_sample)

MESA = r"^(llvmpipe-[0-9]+|[^:]*:disk[$][0-9]+)$"


def stat_line(pid, comm, utime, stime):
    """A /proc/<pid>/stat line with utime and stime in fields 14 and 15."""
    return f"{pid} ({comm}) S 1 {pid} {pid} 0 -1 4194560 100 0 0 0 {utime} {stime} 0 0 20 0 1 0 100 1000 100\n"


def status_text(voluntary, nonvoluntary):
    return (
        "Name:\tTaskSmack\nState:\tS (sleeping)\n"
        f"voluntary_ctxt_switches:\t{voluntary}\nnonvoluntary_ctxt_switches:\t{nonvoluntary}\n"
    )


SMAPS_ROLLUP = """55d0c0000000-7ffc00000000 ---p 00000000 00:00 0                          [rollup]
Rss:              204800 kB
Pss:              153600 kB
Pss_Anon:          81920 kB
Shared_Clean:      40960 kB
Shared_Dirty:          0 kB
Private_Clean:     20480 kB
Private_Dirty:     81920 kB
Referenced:       204800 kB
Anonymous:         81920 kB
Swap:                  0 kB
"""


def make_process(root, pid, comm, threads, ticks=None):
    """A fake /proc/<pid> with the given threads: {tid: (comm, utime, voluntary, nonvoluntary)}."""
    base = root / str(pid)
    (base / "task").mkdir(parents=True, exist_ok=True)
    (base / "fd").mkdir(exist_ok=True)
    for n in range(3):
        (base / "fd" / str(n)).write_text("")
    total = ticks if ticks is not None else sum(t[1] for t in threads.values())
    (base / "stat").write_text(stat_line(pid, comm, total, 0))
    (base / "comm").write_text(comm + "\n")
    (base / "smaps_rollup").write_text(SMAPS_ROLLUP)
    for tid, (name, utime, vol, nonvol) in threads.items():
        task = base / "task" / str(tid)
        task.mkdir(exist_ok=True)
        (task / "stat").write_text(stat_line(tid, name, utime, 0))
        (task / "comm").write_text(name + "\n")
        (task / "status").write_text(status_text(vol, nonvol))


def snap(ticks, threads):
    """A snapshot_process() reading: threads are {tid: (name, ticks, switches)}."""
    return {
        "ticks": ticks,
        "threads": {tid: {"name": n, "ticks": t, "switches": s} for tid, (n, t, s) in threads.items()},
    }


class PhaseSpecTest(unittest.TestCase):
    def test_kinds_and_seconds_in_order(self):
        self.assertEqual(
            idle_sample.parse_phase_spec("visible:120, minimized:60,restored:60"),
            [("visible", 120), ("minimized", 60), ("restored", 60)],
        )

    def test_a_bad_phase_is_refused(self):
        for spec in ("visible:0", "hidden:10", "visible", "visible:-1", "", " , "):
            with self.subTest(spec=spec), self.assertRaises(idle_sample.PhaseSpecError):
                idle_sample.parse_phase_spec(spec)

    def test_the_phases_command_prints_one_line_per_phase(self):
        out = subprocess.run(
            [sys.executable, str(SCRIPT), "phases", "visible:30,minimized:15"], capture_output=True, text=True
        )
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.splitlines(), ["visible 30", "minimized 15"])

    def test_the_phases_command_fails_on_a_bad_spec(self):
        out = subprocess.run([sys.executable, str(SCRIPT), "phases", "visible:0"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 2)
        self.assertIn("Invalid phase 'visible:0'", out.stderr)


class ParserTest(unittest.TestCase):
    def test_stat_ticks_count_from_the_last_paren(self):
        # A thread name may hold spaces and ')': the fields are counted from the last one.
        self.assertEqual(idle_sample.parse_stat_ticks(stat_line(7, "a) b (c", 120, 30)), 150)

    def test_context_switches_add_voluntary_and_nonvoluntary(self):
        self.assertEqual(idle_sample.parse_context_switches(status_text(400, 25)), 425)

    def test_context_switches_without_both_counters_are_none(self):
        self.assertIsNone(idle_sample.parse_context_switches("Name:\tx\nvoluntary_ctxt_switches:\t4\n"))

    def test_smaps_rollup_private_is_clean_plus_dirty(self):
        self.assertEqual(
            idle_sample.parse_smaps_rollup(SMAPS_ROLLUP), {"Rss": 204800, "Pss": 153600, "Private": 102400}
        )

    def test_proc_stat_idle_includes_iowait_and_guest_is_not_counted_twice(self):
        text = "cpu  100 10 50 800 40 5 5 0 30 0\ncpu0 1 2 3 4 5 6 7 8 9 10\n"
        self.assertEqual(idle_sample.parse_proc_stat_cpu(text), (1010, 840))


class SummaryTest(unittest.TestCase):
    def test_threads_cpu_wakeups_and_mesa(self):
        before = snap(1000, {10: ("TaskSmack", 400, 1000), 11: ("llvmpipe-0", 500, 50), 12: ("gone", 100, 7)})
        # Thread 12 exited and 13 started: neither is a row, but the process total still counts them.
        after = snap(1300, {10: ("TaskSmack", 450, 1600), 11: ("llvmpipe-0", 700, 150), 13: ("new", 10, 3)})
        result = idle_sample.summarize_threads(before, after, 10.0, 100, MESA)
        self.assertEqual([r["tid"] for r in result["threads"]], [11, 10])
        self.assertEqual(
            result["threads"][0],
            {"tid": 11, "name": "llvmpipe-0", "module": None, "cpu": 20.0, "wakeupsPerSec": 10.0, "mesa": True},
        )
        self.assertEqual(result["threads"][1]["wakeupsPerSec"], 60.0)
        self.assertEqual(result["totalCpu"], 30.0)
        self.assertEqual(result["mesaCpu"], 20.0)
        self.assertEqual(result["appCpu"], 10.0)
        # Survivors only, as measure-idle.ps1 counts them: (600 + 100) / 10 s.
        self.assertEqual(result["totalWakeupsPerSec"], 70.0)

    def test_unreadable_status_leaves_wakeups_null(self):
        before = snap(0, {10: ("TaskSmack", 0, None)})
        after = snap(100, {10: ("TaskSmack", 100, None)})
        result = idle_sample.summarize_threads(before, after, 1.0, 100, MESA)
        self.assertIsNone(result["threads"][0]["wakeupsPerSec"])
        self.assertIsNone(result["totalWakeupsPerSec"])

    def test_compositor_sums_its_processes(self):
        names = {100: "gnome-shell", 200: "Xwayland"}
        before = {100: snap(1000, {100: ("gnome-shell", 900, 5000)}), 200: snap(50, {200: ("Xwayland", 50, 10)})}
        after = {100: snap(1150, {100: ("gnome-shell", 1050, 5400)}), 200: snap(100, {200: ("Xwayland", 100, 110)})}
        self.assertEqual(
            idle_sample.summarize_compositor(before, after, names, 10.0, 100),
            {"name": "Xwayland,gnome-shell", "cpu": 20.0, "wakeupsPerSec": 50.0},
        )

    def test_no_visible_compositor_is_null(self):
        self.assertIsNone(idle_sample.summarize_compositor({}, {}, {}, 10.0, 100))

    def test_machine_load_takes_out_tasksmack_and_the_compositor(self):
        # 25% of all 4 CPUs busy; TaskSmack 40% and the compositor 20% of one CPU = 15% of all four.
        load = idle_sample.summarize_machine_load((1000, 800), (2000, 1550), 40.0, 20.0, 4)
        self.assertEqual(load, {"busyPctOfAll": 25.0, "restPctOfAll": 10.0})

    def test_machine_load_rest_is_never_negative(self):
        load = idle_sample.summarize_machine_load((0, 0), (1000, 990), 80.0, None, 2)
        self.assertEqual(load["restPctOfAll"], 0.0)

    def test_frames_read_na_as_null(self):
        self.assertEqual(
            idle_sample.frames_from_summary("0", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a"),
            {
                "resizePerfSummaries": 0,
                "fps": None,
                "frameP95Ms": None,
                "frameP99Ms": None,
                "frameMaxMs": None,
                "loopP95Ms": None,
                "loopP99Ms": None,
            },
        )


class FakeProcTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.proc = Path(self.tmp.name) / "proc"
        self.proc.mkdir()
        (self.proc / "stat").write_text("cpu  100 0 100 800 0 0 0 0 0 0\n")
        make_process(self.proc, 4242, "TaskSmack", {4242: ("TaskSmack", 50, 10, 2), 4243: ("llvmpipe-0", 80, 4, 1)})
        make_process(self.proc, 900, "kwin_wayland", {900: ("kwin_wayland", 10, 100, 0)})
        make_process(self.proc, 901, "bash", {901: ("bash", 1, 1, 0)})

    def tearDown(self):
        self.tmp.cleanup()

    def test_memory_reads_smaps_rollup_fds_and_threads(self):
        self.assertEqual(
            idle_sample.read_memory(self.proc, 4242),
            {"privateWorkingSetMB": 100.0, "workingSetMB": 200.0, "pssMB": 150.0, "fds": 3, "threads": 2},
        )

    def test_memory_without_smaps_rollup_is_null_not_an_error(self):
        os.remove(self.proc / "4242" / "smaps_rollup")
        mem = idle_sample.read_memory(self.proc, 4242)
        self.assertIsNone(mem["workingSetMB"])
        self.assertEqual(mem["threads"], 2)

    def test_compositors_are_found_by_comm(self):
        self.assertEqual(idle_sample.find_compositors(self.proc), {900: "kwin_wayland"})

    def test_the_phase_command_prints_measure_idle_ps1s_fields(self):
        rows = Path(self.tmp.name) / "rows.csv"
        out = io.StringIO()
        with redirect_stdout(out):
            rc = idle_sample.main(
                [
                    "phase",
                    "--pid", "4242",
                    "--kind", "visible",
                    "--seconds", "1",
                    "--memory-poll-seconds", "1",
                    "--memory-rows", str(rows),
                    "--rep", "2",
                    "--mesa-regex", MESA,
                    "--minimized", "false",
                    "--proc-root", str(self.proc),
                ]
            )  # fmt: skip
        self.assertEqual(rc, 0)
        phase = json.loads(out.getvalue())
        for key in (
            "phase",
            "durationSeconds",
            "cpuStartUtc",
            "cpuEndUtc",
            "minimized",
            "totalCpu",
            "uiThreadCpu",
            "totalWakeupsPerSec",
            "uiThreadWakeupsPerSec",
            "timerResolutionMs",
            "dwm",
            "machineLoad",
            "frames",
            "memoryStart",
            "memoryEnd",
            "peakPrivateWorkingSetMB",
            "threads",
        ):
            self.assertIn(key, phase)
        self.assertEqual(phase["phase"], "visible")
        self.assertIs(phase["minimized"], False)
        self.assertIsNone(phase["timerResolutionMs"])
        self.assertIsNone(phase["dwm"])
        self.assertEqual(phase["compositor"]["name"], "kwin_wayland")
        self.assertEqual(phase["peakPrivateWorkingSetMB"], 100.0)
        self.assertEqual({t["tid"] for t in phase["threads"]}, {4242, 4243})
        self.assertTrue(all(t["module"] is None for t in phase["threads"]))
        self.assertTrue(phase["cpuStartUtc"].endswith("Z"))
        lines = rows.read_text().splitlines()
        self.assertGreaterEqual(len(lines), 2)
        first = dict(zip(idle_sample.MEMORY_CSV_COLUMNS, lines[0].split(",")))
        self.assertEqual(first["phase"], "visible")
        self.assertEqual(first["workingSetMB"], "200.0")
        self.assertEqual(first["rep"], "2")

    def test_a_process_that_is_gone_exits_3(self):
        out = subprocess.run(
            [sys.executable, str(SCRIPT), "phase", "--pid", "777", "--kind", "visible", "--seconds", "1",
             "--proc-root", str(self.proc)],
            capture_output=True,
            text=True,
        )  # fmt: skip
        self.assertEqual(out.returncode, 3)
        self.assertIn("777", out.stderr)

    def test_set_frames_and_describe(self):
        path = Path(self.tmp.name) / "phase.json"
        out = io.StringIO()
        with redirect_stdout(out):
            idle_sample.main(["phase", "--pid", "4242", "--kind", "restored", "--seconds", "1",
                              "--proc-root", str(self.proc)])  # fmt: skip
        path.write_text(out.getvalue())
        self.assertEqual(
            idle_sample.main(["set-frames", str(path), "3", "21.5", "5.1", "6.2", "7.3", "50.0", "51.0"]), 0
        )
        phase = json.loads(path.read_text())
        self.assertEqual(phase["frames"]["fps"], 21.5)
        self.assertEqual(phase["frames"]["resizePerfSummaries"], 3)
        line = idle_sample.describe_phase(phase)
        self.assertIn("phase restored", line)
        self.assertIn("fps=21.5", line)

    def test_a_skipped_phase_says_why(self):
        phase = idle_sample.skipped_phase("minimized", 60, "the window manager did not minimize the window")
        self.assertEqual(
            phase,
            {
                "phase": "minimized",
                "requestedSeconds": 60,
                "skipped": True,
                "note": "the window manager did not minimize the window",
            },
        )
        self.assertIn("skipped: the window manager", idle_sample.describe_phase(phase))


if __name__ == "__main__":
    unittest.main()
