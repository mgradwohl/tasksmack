#!/usr/bin/env python3
"""Tests for tools/bench-across-launches.py: flagged benchmarks re-measured in separate launches, and the gate
checking the median across them at the same thresholds (#3476, the decision on #3448)."""

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
spec = importlib.util.spec_from_file_location("bench_across_launches", TOOLS / "bench-across-launches.py")
bench_across_launches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench_across_launches)
CHECKER = TOOLS / "check-benchmark-regression.py"


def launch(timings: dict[str, float], unit: str = "ns", repetitions: int = 3, iterations: int = 1000) -> dict:
    """A bench.sh-shaped run: each benchmark's repetitions, then its mean, median, stddev and cv."""
    benchmarks = []
    for name, time in timings.items():
        for index in range(repetitions):
            benchmarks.append(
                {"name": name, "run_name": name, "run_type": "iteration", "repetition_index": index,
                 "iterations": iterations, "real_time": time, "cpu_time": time, "time_unit": unit}
            )
        for aggregate in ("mean", "median", "stddev", "cv"):
            value = time if aggregate in ("mean", "median") else 0.01
            benchmarks.append(
                {"name": f"{name}_{aggregate}", "run_name": name, "run_type": "aggregate", "aggregate_name": aggregate,
                 "iterations": repetitions, "real_time": value, "cpu_time": value, "time_unit": unit}
            )
    return {"context": {"host_name": "redacted"}, "benchmarks": benchmarks}


class BenchAcrossLaunchesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="bench-across-launches-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write(self, name: str, data: object) -> Path:
        path = self.root / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def call(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bench_across_launches.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()

    # ---- plan ----------------------------------------------------------------------------------------------------

    def test_the_filter_matches_exactly_the_flagged_names(self) -> None:
        names = ["BM_HistoryBuffer_CopyTo", "BM_HistoryBuffer_MemoryFootprint/iterations:10000", "BM_Odd.(x)+[y]|z$"]
        pattern = bench_across_launches.benchmark_filter(names)
        self.assertEqual(pattern, r"^(BM_HistoryBuffer_CopyTo|BM_HistoryBuffer_MemoryFootprint/iterations:10000|BM_Odd\.\(x\)\+\[y\]\|z\$)$")
        # Python's re reads these ERE escapes the same way Google Benchmark's std::regex::extended does.
        for name in names:
            self.assertRegex(name, pattern)
        for other in ("BM_HistoryBuffer_CopyToWrapped", "XBM_HistoryBuffer_CopyTo", "BM_Odd.(x)+[y]", "BM_OddX(x)+[y]|z$"):
            self.assertIsNone(re.search(pattern, other), other)

    def test_plan_prints_the_filter_within_the_budget(self) -> None:
        # 3 repetitions x 1000 iterations x 1 ms = 3 s measured, plus 1 s of setup: 4 s a launch, 16 s for 4.
        current = self.write("current.json", launch({"BM_A": 1.0, "BM_B": 1.0}, unit="ms"))
        regressions = self.write("regressions.json", {"regressions": ["BM_A"]})
        code, out, err = self.call("plan", "--current", current, "--regressions", regressions, "--budget-seconds", "16")
        self.assertEqual(code, 0, err)
        self.assertEqual(out.strip(), "^(BM_A)$")
        self.assertIn("about 16s of the 16s budget", err)

    def test_plan_over_budget_leaves_the_flags_standing(self) -> None:
        current = self.write("current.json", launch({"BM_A": 1.0, "BM_B": 1.0}, unit="ms"))
        regressions = self.write("regressions.json", {"regressions": ["BM_A", "BM_B"]})
        code, out, err = self.call("plan", "--current", current, "--regressions", regressions, "--budget-seconds", "31")
        self.assertEqual(code, 1)
        self.assertEqual(out, "", "no filter, so no re-run")
        self.assertIn("flags stand as regressions", err)
        code, _, err = self.call(
            "plan", "--current", current, "--regressions", regressions, "--launches", "1", "--budget-seconds", "8"
        )
        self.assertEqual(code, 0, err)

    def test_plan_with_nothing_flagged_or_bad_input(self) -> None:
        current = self.write("current.json", launch({"BM_A": 1.0}))
        empty = self.write("empty.json", {"regressions": []})
        self.assertEqual(self.call("plan", "--current", current, "--regressions", empty)[0], 1)
        bad = self.write("bad.json", {"regressions": "BM_A"})
        self.assertEqual(self.call("plan", "--current", current, "--regressions", bad)[0], 2)
        self.assertEqual(self.call("plan", "--current", self.root / "missing.json", "--regressions", empty)[0], 2)
        self.assertEqual(self.call("plan", "--current", current)[0], 2)
        self.assertEqual(self.call("plan", "--current", current, "--regressions", empty, "--launches", "0")[0], 2)

    # ---- combine -------------------------------------------------------------------------------------------------

    def combine(self, first: dict, others: list[dict], flagged: list[str], baseline: dict | None = None):
        paths = [self.write(f"launch-{i + 2}.json", data) for i, data in enumerate(others)]
        out = self.root / "combined.json"
        argv = ["combine", "--first", self.write("first.json", first),
                "--regressions", self.write("regressions.json", {"regressions": flagged}), "--output", out]
        if baseline is not None:
            argv += ["--baseline", self.write("baseline.json", baseline)]
        code, stdout, stderr = self.call(*argv, *paths)
        return code, stdout, stderr, (json.loads(out.read_text(encoding="utf-8")) if out.exists() else None)

    def test_each_flagged_benchmark_takes_the_median_of_its_launches(self) -> None:
        first = launch({"BM_A": 150.0, "BM_B": 10.0})
        others = [launch({"BM_A": t}) for t in (101.0, 99.0, 160.0, 100.0)]
        code, out, err, combined = self.combine(first, others, ["BM_A"], baseline=launch({"BM_A": 100.0}))
        self.assertEqual(code, 0, err)
        rows_a = [e for e in combined["benchmarks"] if e["run_name"] == "BM_A"]
        self.assertEqual(len(rows_a), 1, "one median row; the repetitions and other aggregates are dropped")
        self.assertEqual(rows_a[0]["aggregate_name"], "median")
        self.assertEqual(rows_a[0]["real_time"], 101.0, "median of 150, 101, 99, 160, 100")
        self.assertEqual(rows_a[0]["cpu_time"], 101.0)
        self.assertEqual(rows_a[0]["tasksmack_launch_real_times"], [150.0, 101.0, 99.0, 160.0, 100.0])
        self.assertEqual(
            [e for e in combined["benchmarks"] if e["run_name"] == "BM_B"],
            [e for e in first["benchmarks"] if e["run_name"] == "BM_B"],
            "an unflagged benchmark is the first launch's, every row",
        )
        self.assertEqual(combined["context"]["tasksmack_across_launches"], {"launches": 5, "benchmarks": ["BM_A"]})
        # Each launch against the baseline, so a real regression reads as one.
        self.assertIn("BM_A: baseline 100.00ns", out)
        self.assertIn("launch 1: 150.00ns (+50.0%)", out)
        self.assertIn("launch 4: 160.00ns (+60.0%)", out)
        self.assertIn("median: 101.00ns (+1.0%)", out)

    def test_launches_in_other_units_are_combined_in_the_first_launchs_unit(self) -> None:
        first = launch({"BM_A": 2.0}, unit="us")
        others = [launch({"BM_A": t}, unit="ns") for t in (1000.0, 3000.0, 1500.0, 1200.0)]
        code, _, err, combined = self.combine(first, others, ["BM_A"])
        self.assertEqual(code, 0, err)
        row = next(e for e in combined["benchmarks"] if e["run_name"] == "BM_A")
        self.assertEqual(row["time_unit"], "us")
        self.assertAlmostEqual(row["real_time"], 1.5)
        self.assertEqual([round(t, 6) for t in row["tasksmack_launch_real_times"]], [2.0, 1.0, 3.0, 1.5, 1.2])

    def test_a_flagged_benchmark_missing_or_failed_in_a_launch_fails_closed(self) -> None:
        first = launch({"BM_A": 150.0})
        errored = launch({"BM_A": 100.0})
        errored["benchmarks"][0]["error_occurred"] = True
        skipped = {"benchmarks": [{"name": "BM_A", "run_name": "BM_A", "skipped": True, "skip_message": "no GPU"}]}
        nan = launch({"BM_A": float("nan")})
        for bad in (launch({"BM_Other": 1.0}), errored, skipped, nan):
            with self.subTest(bad=bad["benchmarks"][0]):
                code, _, err, combined = self.combine(first, [launch({"BM_A": 100.0}), bad], ["BM_A"])
                self.assertEqual(code, 2)
                self.assertIn("BM_A: no usable median in launch 3", err)
                self.assertIsNone(combined, "nothing for the gate to pass on")

    def test_combine_rejects_bad_input(self) -> None:
        good = self.write("good.json", launch({"BM_A": 1.0}))
        regressions = self.write("regressions.json", {"regressions": ["BM_A"]})
        not_a_run = self.write("not-a-run.json", {"benchmarks": "nope"})
        out = self.root / "out.json"
        self.assertEqual(self.call("combine", "--first", good, "--regressions", regressions, "--output", out, not_a_run)[0], 2)
        self.assertEqual(self.call("combine", "--first", good, "--regressions", regressions, "--output", out)[0], 2)

    # ---- the gate, end to end ------------------------------------------------------------------------------------

    def gate(self, current: Path, baseline: Path) -> tuple[int, str]:
        result = subprocess.run(
            [sys.executable, str(CHECKER), "--baseline", str(baseline), "--current", str(current), "--threshold", "40",
             "--regressions-out", str(self.root / "regressions.json")],
            capture_output=True, encoding="utf-8", env={**os.environ, "PYTHONUTF8": "1"}, check=False,
        )
        return result.returncode, result.stdout + result.stderr

    def across_launches(self, launch_times: list[float]) -> tuple[int, str]:
        """BM_A measured launch_times[0] in the full run and the rest in further launches, against 100 ns."""
        baseline = self.write("baseline.json", launch({"BM_A": 100.0, "BM_B": 50.0, "BM_C": 20.0}))
        first = self.write("first.json", launch({"BM_A": launch_times[0], "BM_B": 50.0, "BM_C": 20.0}))
        code, output = self.gate(first, baseline)
        self.assertEqual(code, 1, output)
        regressions = self.root / "regressions.json"
        self.assertEqual(json.loads(regressions.read_text(encoding="utf-8")), {"regressions": ["BM_A"]})
        others = [self.write(f"launch-{i + 2}.json", launch({"BM_A": t})) for i, t in enumerate(launch_times[1:])]
        combined = self.root / "combined.json"
        code, _, err = self.call("combine", "--first", first, "--regressions", regressions, "--output", combined, *others)
        self.assertEqual(code, 0, err)
        return self.gate(combined, baseline)

    def test_one_slow_launch_is_not_a_regression(self) -> None:
        # #3476: a 4 ns benchmark read +52% in one launch of an unchanged binary.
        code, output = self.across_launches([152.0, 101.0, 98.0, 149.0, 103.0])
        self.assertEqual(code, 0, output)
        self.assertIn("Coverage: 100.0% (3 of 3", output, "the re-check still covers the whole suite")

    def test_a_real_regression_stays_red(self) -> None:
        # Slow in every launch, or in most of them: the median is past the threshold.
        for times in ([160.0, 158.0, 162.0, 155.0, 161.0], [160.0, 101.0, 150.0, 99.0, 145.0]):
            with self.subTest(times=times):
                code, output = self.across_launches(times)
                self.assertEqual(code, 1, output)
                self.assertIn("BM_A", output)


if __name__ == "__main__":
    unittest.main()
