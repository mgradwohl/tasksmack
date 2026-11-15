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
import time
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

    def test_plan_counts_a_manual_time_benchmark_conservatively(self) -> None:
        # #3508: a /manual_time benchmark reports one handoff's median of 11 per iteration, about 4 ms of
        # repetitions on CI for 1.1-1.3 s of wall time. 3 x 1000 x 1 us = 3 ms, so 25 x 3 ms + 5 s = 5.075 s a launch.
        name = "BM_SystemModel_Concurrent_PublicationWait/samples:18000/extra_readers:2/iterations:2000/manual_time"
        current = self.write("current.json", launch({name: 1.0}, unit="us"))
        regressions = self.write("regressions.json", {"regressions": [name]})
        code, _, err = self.call("plan", "--current", current, "--regressions", regressions, "--budget-seconds", "20")
        self.assertEqual(code, 1, "4 launches of 5.075 s are over 20 s")
        self.assertIn("about 5s a launch", err)
        code, out, err = self.call("plan", "--current", current, "--regressions", regressions, "--budget-seconds", "21")
        self.assertEqual(code, 0, err)
        self.assertIn("manual_time", out)

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

    def test_exclude_first_takes_the_medians_from_the_second_runner_alone(self) -> None:
        # #3623: the first runner was a slower CPU, slow in every launch; the second runner's launches decide.
        first = launch({"BM_A": 150.0, "BM_B": 10.0})
        first["context"] = {"host_name": "redacted", "mhz_per_cpu": 2300}
        others = [launch({"BM_A": t}) for t in (101.0, 99.0, 160.0, 100.0, 102.0)]
        others[0]["context"] = {"host_name": "redacted", "mhz_per_cpu": 3243}
        paths = [self.write(f"second-{i + 1}.json", data) for i, data in enumerate(others)]
        out = self.root / "combined.json"
        code, stdout, err = self.call(
            "combine", "--first", self.write("first.json", first), "--exclude-first",
            "--regressions", self.write("regressions.json", {"regressions": ["BM_A"]}), "--output", out,
            "--baseline", self.write("baseline.json", launch({"BM_A": 100.0})), *paths)
        self.assertEqual(code, 0, err)
        combined = json.loads(out.read_text(encoding="utf-8"))
        row = next(e for e in combined["benchmarks"] if e["run_name"] == "BM_A")
        self.assertEqual(row["real_time"], 101.0, "median of 101, 99, 160, 100, 102: the first runner's 150 not counted")
        self.assertEqual(row["tasksmack_launch_real_times"], [101.0, 99.0, 160.0, 100.0, 102.0])
        self.assertEqual(
            [e for e in combined["benchmarks"] if e["run_name"] == "BM_B"],
            [e for e in first["benchmarks"] if e["run_name"] == "BM_B"],
            "an unflagged benchmark is still the full run's, for the same coverage",
        )
        self.assertEqual(combined["context"]["mhz_per_cpu"], 3243, "the context of the machine that measured them")
        self.assertEqual(combined["context"]["tasksmack_across_launches"],
                         {"launches": 5, "benchmarks": ["BM_A"], "first_counted": False})
        self.assertIn("Median across 5 separate launches on this runner (the first runner's values not counted)", stdout)
        self.assertIn("launch 1: 101.00ns (+1.0%)", stdout)
        self.assertNotIn("150.00ns", stdout)

    def test_exclude_first_still_fails_closed_on_a_missing_launch(self) -> None:
        first = launch({"BM_A": 150.0})
        paths = [self.write("second-1.json", launch({"BM_A": 100.0})), self.write("second-2.json", launch({"BM_B": 1.0}))]
        code, _, err = self.call(
            "combine", "--first", self.write("first.json", first), "--exclude-first",
            "--regressions", self.write("regressions.json", {"regressions": ["BM_A"]}),
            "--output", self.root / "combined.json", *paths)
        self.assertEqual(code, 2)
        self.assertIn("BM_A: no usable median in launch 2", err)

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


LAUNCH_STUB = r"""
import json, os, re, sys, time
from pathlib import Path

# bench.sh's stand-in for run (#3830): one launch per call, numbered by a counter, its timings, exit code, sleep
# and core class from plan.json. Writes what the filter selects, shaped as bench.sh writes it.
stub = Path(__file__).parent
plan = json.loads((stub / "plan.json").read_text(encoding="utf-8"))
number = int((stub / "count").read_text(encoding="utf-8")) + 1
(stub / "count").write_text(str(number), encoding="utf-8")
args = sys.argv[1:]
with open(stub / "calls.log", "a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\n")
spec = plan[str(number)]
print(f"launch {number} running")
sys.stdout.flush()
time.sleep(spec.get("sleep", 0))
if spec.get("exit"):
    sys.exit(spec["exit"])
pattern = [a for a in args if a.startswith("--benchmark_filter=")][-1].split("=", 1)[1]
out = Path(os.environ["TASKSMACK_BENCH_OUT_DIR"]) / f"bench-{number}.json"
rows = []
for name, value in spec.get("times", {}).items():
    if re.search(pattern, name) is None:
        continue
    rows += [{"name": name, "run_name": name, "run_type": "iteration", "repetition_index": i, "iterations": 10,
              "real_time": value, "cpu_time": value, "time_unit": "ns"} for i in range(3)]
    rows += [{"name": f"{name}_{a}", "run_name": name, "run_type": "aggregate", "aggregate_name": a,
              "real_time": value, "cpu_time": value, "time_unit": "ns"} for a in ("mean", "median")]
if not spec.get("no_result"):
    out.write_text(json.dumps({"context": {"host_name": "redacted"}, "benchmarks": rows}), encoding="utf-8")
    out.with_suffix(".manifest.json").write_text(
        json.dumps({"placement": {"core_class": spec.get("core_class", "any")}}), encoding="utf-8")
print(f"RESULT={out}")
"""


class RunTest(unittest.TestCase):
    """run (#3830, #3912): the plan, the further launches and the combine in one place, for both heavy-checks.yml
    benchmark jobs and bench.sh --baseline."""

    BASELINE = {"BM_A": 100.0, "BM_B": 50.0}

    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="bench-across-launches-run-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.stub = self.root / "stub"
        self.stub.mkdir()
        (self.stub / "launch.py").write_text(LAUNCH_STUB, encoding="utf-8")
        (self.stub / "count").write_text("0", encoding="utf-8")
        self.baseline = self.write("baseline.json", launch(self.BASELINE))
        self.first = self.write("first.json", launch({"BM_A": 160.0, "BM_B": 50.0}))
        self.regressions = self.write("regressions.json", {"regressions": ["BM_A"]})
        self.output = self.root / "combined.json"
        self.work = self.root / "launches"
        self.report = self.root / "report.txt"

    def write(self, name: str, data: object) -> Path:
        path = self.root / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def plan(self, launches: dict[int, dict], first_number: int = 2) -> None:
        (self.stub / "plan.json").write_text(json.dumps({str(n): spec for n, spec in launches.items()}), encoding="utf-8")
        (self.stub / "count").write_text(str(first_number - 1), encoding="utf-8")

    def run_tool(self, *extra: str, budget: str = "600") -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        argv = ["run", "--current", self.first, "--regressions", self.regressions, "--baseline", self.baseline,
                "--output", self.output, "--work-dir", self.work, "--budget-seconds", budget, "--report", self.report,
                *extra, "--", sys.executable, self.stub / "launch.py", "--", "--benchmark_filter=Other"]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bench_across_launches.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()

    def calls(self) -> list[list[str]]:
        log = self.stub / "calls.log"
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()] if log.exists() else []

    def report_text(self) -> str:
        return self.report.read_text(encoding="utf-8") if self.report.exists() else ""

    def test_the_flagged_benchmark_runs_in_four_more_launches_and_takes_their_median(self) -> None:
        self.plan({n: {"times": {"BM_A": t, "BM_B": 50.0}} for n, t in zip(range(2, 6), (101.0, 99.0, 158.0, 102.0))})
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        calls = self.calls()
        self.assertEqual(len(calls), 4)
        for call in calls:
            # The command's own arguments, then the plan's filter last, which Google Benchmark takes.
            self.assertEqual(call, ["--", "--benchmark_filter=Other", "--benchmark_filter=^(BM_A)$"])
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ["2", "3", "4", "5"])
        combined = json.loads(self.output.read_text(encoding="utf-8"))
        rows = [r for r in combined["benchmarks"] if r["run_name"] == "BM_A"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["real_time"], 102.0, "the median of 160, 101, 99, 158, 102")
        self.assertEqual(rows[0]["tasksmack_launch_real_times"], [160.0, 101.0, 99.0, 158.0, 102.0])
        report = self.report_text()
        self.assertIn("1 flagged benchmark(s), about", report)
        self.assertIn("launch 1: 160.00ns (+60.0%)", report)
        self.assertIn("median: 102.00ns (+2.0%)", report)
        # Each launch's own output reaches the console, not the report.
        self.assertIn("launch 3 running", out)
        self.assertNotIn("launch 3 running", report)

    def test_exclude_first_numbers_the_launches_from_1_and_combines_onto_first(self) -> None:
        self.plan({n: {"times": {"BM_A": 101.0}} for n in range(1, 6)}, first_number=1)
        judged = self.write("judged.json", launch({"BM_A": 150.0, "BM_B": 51.0}))
        code, out, err = self.run_tool("--exclude-first", "--first", str(judged), "--launches", "5")
        self.assertEqual(code, 0, out + err)
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ["1", "2", "3", "4", "5"])
        combined = json.loads(self.output.read_text(encoding="utf-8"))
        times = {r["run_name"]: r["real_time"] for r in combined["benchmarks"] if r.get("aggregate_name") == "median"}
        self.assertEqual(times, {"BM_A": 101.0, "BM_B": 51.0}, "the flagged one from the launches, the rest from --first")
        self.assertIn("Median across 5 separate launches on this runner", self.report_text())

    def test_nothing_flagged_or_over_budget_starts_no_launch(self) -> None:
        self.plan({})
        self.regressions.write_text(json.dumps({"regressions": []}), encoding="utf-8")
        code, out, err = self.run_tool()
        self.assertEqual(code, 1, out + err)
        self.assertIn("Nothing to re-measure", self.report_text())
        self.regressions.write_text(json.dumps({"regressions": ["BM_A"]}), encoding="utf-8")
        # 1 s of setup a launch for a benchmark with 30 ns of repetitions: 4 s for 4, over 3.
        code, out, err = self.run_tool(budget="3")
        self.assertEqual(code, 1, out + err)
        self.assertIn("flags stand as regressions", self.report_text())
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.output.exists())

    def test_the_deadline_passing_between_launches_stops_them(self) -> None:
        # The clock reads once at the start, then before each launch: launch 2 has the whole budget, launch 3 none.
        self.plan({n: {"times": {"BM_A": 101.0}} for n in range(2, 6)})
        ticks = iter([0.0, 0.0, 600.0])
        original = bench_across_launches.clock
        bench_across_launches.clock = lambda: next(ticks)
        self.addCleanup(setattr, bench_across_launches, "clock", original)
        code, out, err = self.run_tool()
        self.assertEqual(code, 1, out + err)
        self.assertEqual(len(self.calls()), 1, "launch 2 only")
        self.assertIn("Out of time: launch 3 of 5 not started, past the 600s budget. The flags stand as regressions.",
                      self.report_text())
        self.assertFalse(self.output.exists())

    def test_a_launch_past_the_deadline_is_stopped_and_its_output_deleted(self) -> None:
        # The plan's estimate is 1 s a launch, 4 s for 4: within 5 s, and launch 2 sleeps past it.
        self.plan({2: {"times": {"BM_A": 101.0}, "sleep": 30}, **{n: {"times": {"BM_A": 101.0}} for n in range(3, 6)}})
        started = time.monotonic()
        code, out, err = self.run_tool(budget="5")
        self.assertLess(time.monotonic() - started, 20, "stopped, not waited out")
        self.assertEqual(code, 1, out + err)
        self.assertIn("Out of time: launch 2 of 5 stopped after 5s, past the 5s budget.", self.report_text())
        self.assertFalse((self.work / "2").exists(), "a stopped launch's partial output")
        self.assertEqual(len(self.calls()), 1)

    def test_a_failed_launch_or_one_with_no_result_is_exit_3(self) -> None:
        for spec, message in (({"exit": 7}, "launch 3 failed (exit 7)"), ({"no_result": True}, "launch 3's output JSON not found")):
            with self.subTest(spec=spec):
                self.plan({2: {"times": {"BM_A": 101.0}}, 3: spec, 4: {"times": {"BM_A": 101.0}}, 5: {"times": {"BM_A": 101.0}}})
                self.report.unlink(missing_ok=True)
                code, out, err = self.run_tool()
                self.assertEqual(code, 3, out + err)
                self.assertIn(message, err)
                self.assertIn(message, self.report_text())
                self.assertEqual(len(self.calls()), 2, "launches 2 and 3, none after the failure")
                (self.stub / "calls.log").unlink()

    def test_a_launch_missing_the_benchmark_fails_the_combine(self) -> None:
        self.plan({2: {"times": {"BM_A": 101.0}}, 3: {"times": {}}, 4: {"times": {"BM_A": 101.0}}, 5: {"times": {"BM_A": 101.0}}})
        code, out, err = self.run_tool()
        self.assertEqual(code, 2, out + err)
        self.assertIn("no usable median in launch 3", self.report_text())

    def test_a_launch_on_another_core_class_is_not_combined(self) -> None:
        # bench.ps1 -Baseline's check (#3163): a launch whose pinning went elsewhere can't share a median.
        manifest = self.write("first.manifest.json", {"placement": {"core_class": "performance"}})
        self.plan({2: {"times": {"BM_A": 101.0}, "core_class": "performance"},
                   3: {"times": {"BM_A": 101.0}, "core_class": "any"}})
        code, out, err = self.run_tool("--first-manifest", str(manifest))
        self.assertEqual(code, 2, out + err)
        self.assertIn("launch 3 ran on core class any, not the full run's performance", err)
        self.assertFalse(self.output.exists())

    def test_run_needs_the_launch_command(self) -> None:
        for tail in ([], ["--"]):
            with self.subTest(tail=tail):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    code = bench_across_launches.main(
                        ["run", "--current", str(self.first), "--regressions", str(self.regressions), "--baseline",
                         str(self.baseline), "--output", str(self.output), "--work-dir", str(self.work), *tail])
                self.assertEqual(code, 2)
                self.assertIn("launch command", err.getvalue())

    def test_unusable_input_is_exit_2_and_reported(self) -> None:
        self.regressions.write_text("not json", encoding="utf-8")
        code, out, err = self.run_tool()
        self.assertEqual(code, 2, out + err)
        self.assertIn("ERROR:", self.report_text())


if __name__ == "__main__":
    unittest.main()
