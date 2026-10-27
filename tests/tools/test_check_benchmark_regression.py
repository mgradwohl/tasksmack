#!/usr/bin/env python3
"""Tests for tools/check-benchmark-regression.py's regression gate (#1322 noise floor, #1420 skips)."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "check-benchmark-regression.py"


def write_run(directory: Path, name: str, timings: dict[str, float]) -> Path:
    """Write a Google Benchmark JSON file holding one median aggregate per benchmark."""
    benchmarks = [
        {
            "name": f"{bm}_median",
            "run_name": bm,
            "aggregate_name": "median",
            "real_time": ns,
            "cpu_time": ns,
            "time_unit": "ns",
        }
        for bm, ns in timings.items()
    ]
    path = directory / name
    path.write_text(json.dumps({"benchmarks": benchmarks}), encoding="utf-8")
    return path


def skipped_records(bm: str, repetitions: int = 3, *, error: bool = False) -> list[dict]:
    """The records Google Benchmark writes for a benchmark that skips itself on every repetition:
    one per repetition, no aggregates, zero timings. SkipWithMessage() writes "skipped" and
    "skip_message"; SkipWithError() writes "error_occurred" and "error_message"."""
    flag = {"error_occurred": True, "error_message": "probe failed"} if error else {"skipped": True, "skip_message": "No GPUs available"}
    return [
        {
            "name": bm,
            "run_name": bm,
            "run_type": "iteration",
            "repetition_index": index,
            **flag,
            "iterations": 0,
            "real_time": 0.0,
            "cpu_time": 0.0,
            "time_unit": "ns",
        }
        for index in range(repetitions)
    ]


def add_records(path: Path, records: list[dict]) -> None:
    """Append raw benchmark records to a file write_run() wrote."""
    data = json.loads(path.read_text(encoding="utf-8"))
    data["benchmarks"].extend(records)
    path.write_text(json.dumps(data), encoding="utf-8")


class CheckBenchmarkRegressionTest(unittest.TestCase):
    def run_gate(
        self,
        baseline: dict[str, float],
        current: dict[str, float],
        *extra: str,
        current_extra: list[dict] | None = None,
        baseline_extra: list[dict] | None = None,
        overrides: object = None,
    ):
        """overrides, when given, is written as the --threshold-overrides file: a str verbatim,
        anything else as JSON."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            current_path = write_run(tmp_path, "current.json", current)
            if current_extra:
                add_records(current_path, current_extra)
            baseline_path = write_run(tmp_path, "baseline.json", baseline)
            if baseline_extra:
                add_records(baseline_path, baseline_extra)
            if overrides is not None:
                overrides_path = tmp_path / "overrides.json"
                text = overrides if isinstance(overrides, str) else json.dumps(overrides)
                overrides_path.write_text(text, encoding="utf-8")
                extra = (*extra, "--threshold-overrides", str(overrides_path))
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--baseline",
                    str(baseline_path),
                    "--current",
                    str(current_path),
                    "--threshold",
                    "40",
                    *extra,
                ],
                capture_output=True,
                encoding="utf-8",
                env={**os.environ, "PYTHONUTF8": "1"},
                check=False,
            )
        return result.returncode, result.stdout + result.stderr

    def test_sub_nanosecond_noise_is_not_a_regression(self):
        # BM_Numeric_ToDouble_Int in #1322: 0.4ns -> 0.6ns reads +55% but is 0.2ns of noise.
        code, output = self.run_gate({"BM_Tiny": 0.4, "BM_Big": 100.0}, {"BM_Tiny": 0.6, "BM_Big": 100.0})
        self.assertEqual(code, 0, output)
        self.assertIn("noise floor", output)
        # The success line must not claim every benchmark was within the percentage threshold.
        self.assertNotIn("within 40.0% threshold", output)
        self.assertIn("No benchmark exceeded both the 40.0% threshold and the 1.00ns floor", output)
        self.assertIn("1 over 40.0% only within the noise floor", output)

    def test_large_absolute_regression_still_fails(self):
        # BM_GPUModel_ProcessGpuCounters in #1322: 12.2ns -> 19.3ns is a real 7ns slowdown.
        code, output = self.run_gate({"BM_Tiny": 0.4, "BM_Big": 12.2}, {"BM_Tiny": 0.4, "BM_Big": 19.3})
        self.assertEqual(code, 1, output)
        self.assertIn("BM_Big", output)

    def test_floor_of_zero_gates_on_percentage_alone(self):
        code, output = self.run_gate({"BM_Tiny": 0.4}, {"BM_Tiny": 0.6}, "--min-abs-delta-ns", "0")
        self.assertEqual(code, 1, output)

    def test_slowdown_under_threshold_passes(self):
        code, output = self.run_gate({"BM_Big": 100.0}, {"BM_Big": 130.0})
        self.assertEqual(code, 0, output)

    def test_raw_repetitions_compare_on_the_median(self):
        # #1424: bench.sh keeps every repetition. A slow outlier repetition sharing the median's
        # run_name must not replace the median, whichever order the rows come in.
        def row(ns: float, aggregate: str | None = None) -> dict:
            record = {"name": "BM_Big", "run_name": "BM_Big", "real_time": ns, "time_unit": "ns"}
            if aggregate:
                record.update(name=f"BM_Big_{aggregate}", run_type="aggregate", aggregate_name=aggregate)
            else:
                record.update(run_type="iteration")
            return record

        for rows in (
            [row(100.0), row(1000.0), row(100.0, "mean"), row(100.0, "median")],
            [row(100.0, "median"), row(1000.0), row(100.0, "mean")],
        ):
            with tempfile.TemporaryDirectory() as tmp:
                tmp_path = Path(tmp)
                current = tmp_path / "current.json"
                current.write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")
                result = subprocess.run(
                    [
                        sys.executable,
                        str(SCRIPT),
                        "--baseline",
                        str(write_run(tmp_path, "baseline.json", {"BM_Big": 100.0})),
                        "--current",
                        str(current),
                        "--threshold",
                        "40",
                    ],
                    capture_output=True,
                    encoding="utf-8",
                    env={**os.environ, "PYTHONUTF8": "1"},
                    check=False,
                )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_an_errored_repetition_is_not_hidden_by_the_median(self):
        # #1445 review: one repetition failed (SkipWithError) while the others produced a median.
        # The failure must win over the median in either row order, so the benchmark still
        # counts against coverage (7 of 8 compared = 87.5%) instead of passing at 100%.
        ok = {"name": "BM_Big", "run_name": "BM_Big", "run_type": "iteration", "real_time": 100.0, "time_unit": "ns"}
        error = {**ok, "error_occurred": True, "error_message": "probe failed", "real_time": 0.0}
        median = {**ok, "name": "BM_Big_median", "run_type": "aggregate", "aggregate_name": "median"}
        baseline = {**self.OTHER_BENCHMARKS, "BM_Big": 100.0}
        for rows in ([ok, error, median], [median, error, ok], [error, median, ok], [median, ok, error]):
            with self.subTest(order=[row.get("aggregate_name") or ("error" if "error_occurred" in row else "ok") for row in rows]):
                code, output = self.run_gate(baseline, self.OTHER_BENCHMARKS, "--min-coverage", "90", current_extra=rows)
                self.assertEqual(code, 1, output)
                self.assertIn("coverage 87.5% is below the required 90.0%", output)

    def test_the_median_still_beats_successful_repetitions(self):
        # The error precedence above does not change the successful case: a slow successful
        # repetition next to the median is still not compared.
        ok = {"name": "BM_Big", "run_name": "BM_Big", "run_type": "iteration", "real_time": 1000.0, "time_unit": "ns"}
        median = {**ok, "name": "BM_Big_median", "run_type": "aggregate", "aggregate_name": "median", "real_time": 100.0}
        baseline = {**self.OTHER_BENCHMARKS, "BM_Big": 100.0}
        for rows in ([ok, median], [median, ok]):
            code, output = self.run_gate(baseline, self.OTHER_BENCHMARKS, current_extra=rows)
            self.assertEqual(code, 0, output)

    def test_non_finite_floor_is_a_usage_error(self):
        code, output = self.run_gate({"BM_Big": 100.0}, {"BM_Big": 100.0}, "--min-abs-delta-ns", "nan")
        self.assertEqual(code, 2, output)

    # #1420: the GPU benchmarks skip themselves on a GPU-less runner rather than time an empty probe.
    GPU_BENCHMARKS = ("BM_GPUModel_Refresh", "BM_GPUModel_Snapshots", "BM_GPUProbe_Enumerate")
    OTHER_BENCHMARKS = {f"BM_Other_{index}": 100.0 for index in range(7)}

    def test_a_skipped_benchmark_is_not_measured_and_not_counted_against_coverage(self):
        baseline = {**self.OTHER_BENCHMARKS, **{bm: 40.0 for bm in self.GPU_BENCHMARKS}}
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        # 7 of 10 baseline benchmarks compared would be 70% coverage; the 3 skips are out of the
        # denominator, so it is 7 of 7.
        code, output = self.run_gate(baseline, self.OTHER_BENCHMARKS, "--min-coverage", "90", current_extra=skipped)
        self.assertEqual(code, 0, output)
        self.assertIn("Not measured (3)", output)
        for bm in self.GPU_BENCHMARKS:
            self.assertIn(f"{bm}: skipped in current run: No GPUs available", output)
        self.assertIn("coverage 100.0% (3 skipped on purpose, not counted)", output)
        self.assertNotIn("Invalid/unusable", output)

    def test_a_skipped_benchmark_does_not_hide_a_real_regression(self):
        baseline = {**self.OTHER_BENCHMARKS, **{bm: 40.0 for bm in self.GPU_BENCHMARKS}}
        current = {**self.OTHER_BENCHMARKS, "BM_Other_0": 200.0}
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        code, output = self.run_gate(baseline, current, current_extra=skipped)
        self.assertEqual(code, 1, output)
        self.assertIn("BM_Other_0", output)

    def test_a_benchmark_skipped_in_the_baseline_and_absent_now_is_not_counted(self):
        # The GPU benchmarks skipped when the baseline was recorded and are absent from this run:
        # not a coverage loss either (#1439 review). 7 of 10 would read 70%; it is 7 of 7.
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        code, output = self.run_gate(self.OTHER_BENCHMARKS, self.OTHER_BENCHMARKS, "--min-coverage", "90", baseline_extra=skipped)
        self.assertEqual(code, 0, output)
        self.assertIn("Not measured (3)", output)
        for bm in self.GPU_BENCHMARKS:
            self.assertIn(f"{bm}: skipped in baseline run: No GPUs available", output)
        self.assertNotIn("Missing from current run", output)
        self.assertIn("coverage 100.0% (3 skipped on purpose, not counted)", output)

    def test_a_benchmark_skipped_in_the_baseline_but_measured_now_is_not_counted(self):
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        current = {**self.OTHER_BENCHMARKS, **{bm: 40.0 for bm in self.GPU_BENCHMARKS}}
        code, output = self.run_gate(self.OTHER_BENCHMARKS, current, "--min-coverage", "90", baseline_extra=skipped)
        self.assertEqual(code, 0, output)
        self.assertIn("Not measured (3)", output)
        self.assertIn("coverage 100.0%", output)

    def test_a_benchmark_missing_from_the_current_run_still_counts_against_coverage(self):
        baseline = {**self.OTHER_BENCHMARKS, **{bm: 40.0 for bm in self.GPU_BENCHMARKS}}
        code, output = self.run_gate(baseline, self.OTHER_BENCHMARKS, "--min-coverage", "90")
        self.assertEqual(code, 1, output)
        self.assertIn("Missing from current run (3)", output)
        self.assertIn("coverage 70.0% is below the required 90.0%", output)

    def test_a_current_error_is_not_hidden_by_a_baseline_skip(self):
        # #1439 review: a baseline recorded with no GPU (skipped) and a current run whose GPU
        # benchmarks failed (SkipWithError) must not pass as "not measured" at 100% coverage.
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        errored = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm, error=True)]
        code, output = self.run_gate(
            self.OTHER_BENCHMARKS, self.OTHER_BENCHMARKS, "--min-coverage", "90", baseline_extra=skipped, current_extra=errored
        )
        self.assertEqual(code, 1, output)
        self.assertNotIn("Not measured", output)
        self.assertIn("coverage 70.0% is below the required 90.0%", output)

    def test_a_baseline_error_is_not_hidden_by_a_current_skip(self):
        errored = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm, error=True)]
        skipped = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm)]
        code, output = self.run_gate(
            self.OTHER_BENCHMARKS, self.OTHER_BENCHMARKS, "--min-coverage", "90", baseline_extra=errored, current_extra=skipped
        )
        self.assertEqual(code, 1, output)
        self.assertNotIn("Not measured", output)
        self.assertIn("coverage 70.0% is below the required 90.0%", output)

    def test_a_skipped_benchmark_only_in_the_current_run_is_not_measured_not_new(self):
        # #1439 review: a new benchmark that skips itself is reported as not measured, like a skip
        # on either side, and leaves coverage (over the baseline) untouched.
        skipped = skipped_records("BM_GPUModel_New")
        code, output = self.run_gate(self.OTHER_BENCHMARKS, self.OTHER_BENCHMARKS, "--min-coverage", "90", current_extra=skipped)
        self.assertEqual(code, 0, output)
        self.assertIn("Not measured (1)", output)
        self.assertIn("BM_GPUModel_New: skipped in current run: No GPUs available", output)
        self.assertNotIn("New in current run", output)
        self.assertIn("7 compared; coverage 100.0%.", output)

    def test_a_benchmark_that_errored_still_counts_against_coverage(self):
        baseline = {**self.OTHER_BENCHMARKS, **{bm: 40.0 for bm in self.GPU_BENCHMARKS}}
        errored = [record for bm in self.GPU_BENCHMARKS for record in skipped_records(bm, error=True)]
        code, output = self.run_gate(baseline, self.OTHER_BENCHMARKS, "--min-coverage", "90", current_extra=errored)
        self.assertEqual(code, 1, output)
        self.assertIn("coverage 70.0% is below the required 90.0%", output)
        self.assertNotIn("Not measured", output)

    # #2336: per-benchmark thresholds for the runner-class-sensitive (allocator-bound) benchmarks.
    @staticmethod
    def override(threshold: object, name: str = "BM_Memory", reason: object = "allocator-bound") -> dict:
        return {"thresholds": {name: {"threshold": threshold, "reason": reason}}}

    def test_an_override_loosens_only_its_own_benchmark(self):
        # BM_ProcessModel_GetSnapshots on run 38853886185: +40.4% with no code change.
        baseline = {"BM_Memory": 7490.0, "BM_Compute": 100.0}
        code, output = self.run_gate(baseline, {"BM_Memory": 10518.0, "BM_Compute": 100.0}, overrides=self.override(60))
        self.assertEqual(code, 0, output)
        self.assertIn("Per-benchmark thresholds (1, from", output)
        self.assertIn("BM_Memory: 60.0%", output)
        self.assertIn("40.0% threshold (1 with their own)", output)
        # The other benchmarks keep --threshold.
        code, output = self.run_gate(baseline, {"BM_Memory": 7490.0, "BM_Compute": 145.0}, overrides=self.override(60))
        self.assertEqual(code, 1, output)
        self.assertIn("BM_Compute", output)

    def test_an_override_still_catches_a_2x_regression(self):
        code, output = self.run_gate({"BM_Memory": 7490.0}, {"BM_Memory": 14980.0}, overrides=self.override(60))
        self.assertEqual(code, 1, output)
        self.assertIn("BM_Memory: 7490.0ns -> 14980.0ns  (+100.0%)  [threshold 60.0%]", output)
        self.assertIn("regressed beyond 40.0% (or their own threshold)", output)

    def test_an_override_can_be_stricter(self):
        code, output = self.run_gate({"BM_Memory": 100.0}, {"BM_Memory": 120.0}, overrides=self.override(10))
        self.assertEqual(code, 1, output)

    def test_without_the_flag_every_benchmark_uses_the_threshold(self):
        code, output = self.run_gate({"BM_Memory": 7490.0}, {"BM_Memory": 10518.0})
        self.assertEqual(code, 1, output)
        self.assertNotIn("Per-benchmark thresholds", output)

    def test_an_override_for_a_benchmark_not_in_the_baseline_is_reported(self):
        code, output = self.run_gate({"BM_Other": 100.0}, {"BM_Other": 100.0}, overrides=self.override(60))
        self.assertEqual(code, 0, output)
        self.assertIn("BM_Memory: 60.0%  (not in baseline, ignored)", output)

    def test_an_invalid_overrides_file_is_a_usage_error(self):
        cases = {
            "nan threshold": self.override(float("nan")),
            "negative threshold": self.override(-1),
            "bool threshold": self.override(True),
            "string threshold": self.override("60"),
            "missing reason": {"thresholds": {"BM_Memory": {"threshold": 60}}},
            "blank reason": self.override(60, reason="  "),
            "bare number": {"thresholds": {"BM_Memory": 60}},
            "no thresholds object": {"BM_Memory": {"threshold": 60, "reason": "x"}},
            "empty name": self.override(60, name=""),
            "not JSON": "{",
        }
        for label, overrides in cases.items():
            with self.subTest(label):
                code, output = self.run_gate({"BM_Memory": 100.0}, {"BM_Memory": 100.0}, overrides=overrides)
                self.assertEqual(code, 2, output)
                self.assertIn("invalid threshold overrides", output)

    def test_a_missing_overrides_file_is_a_usage_error(self):
        code, output = self.run_gate(
            {"BM_Memory": 100.0}, {"BM_Memory": 100.0}, "--threshold-overrides", "does-not-exist.json"
        )
        self.assertEqual(code, 2, output)
        self.assertIn("threshold overrides file not found", output)

    def test_the_committed_ci_overrides_are_valid_and_name_baseline_benchmarks(self):
        perf_data = SCRIPT.parents[1] / "perf-data"
        overrides = json.loads((perf_data / "linux-ci-thresholds.json").read_text(encoding="utf-8"))["thresholds"]
        baseline = json.loads((perf_data / "linux-ci-baseline.json").read_text(encoding="utf-8"))["benchmarks"]
        baseline_names = {bm.get("run_name") or bm.get("name") for bm in baseline}
        self.assertTrue(overrides)
        for name, entry in overrides.items():
            with self.subTest(name):
                self.assertIn(name, baseline_names)
                # Each CI override loosens a limit heavy-checks.yml passes (40%, the 1.0ns default).
                self.assertTrue("threshold" in entry or "min_abs_delta_ns" in entry)
                if "threshold" in entry:
                    self.assertGreater(entry["threshold"], 40)
                if "min_abs_delta_ns" in entry:
                    self.assertGreater(entry["min_abs_delta_ns"], 1.0)
                self.assertTrue(entry["reason"].strip())

    # #3126: a per-benchmark noise floor for a sub-nanosecond benchmark that code layout moves.
    @staticmethod
    def floor_override(floor: object, name: str = "BM_Format_ChooseByteUnit", **extra: object) -> dict:
        return {"thresholds": {name: {"min_abs_delta_ns": floor, "reason": "code layout", **extra}}}

    def test_a_floor_override_decides_each_case(self):
        # Gate at 40% with the 1.0ns default floor and BM_Format_ChooseByteUnit's own 3ns floor. The
        # first row is runs 40444757786 / 40448017472; the others show the floor is no blanket pass.
        cases = [
            ("layout noise on the floored benchmark", "BM_Format_ChooseByteUnit", 0.7, 3.1, 0),
            ("the floored benchmark over its floor", "BM_Format_ChooseByteUnit", 0.7, 4.0, 1),
            ("the floored benchmark at 2ns -> 12ns", "BM_Format_ChooseByteUnit", 2.0, 12.0, 1),
            ("another sub-ns benchmark keeps 1.0ns", "BM_Numeric_ToDouble_Int", 0.7, 3.1, 1),
            ("a 10ns -> 40ns regression elsewhere", "BM_Numeric_ToDouble_Int", 10.0, 40.0, 1),
            ("2ns -> 12ns elsewhere", "BM_Numeric_ToDouble_Int", 2.0, 12.0, 1),
        ]
        for label, name, base_ns, cur_ns, expected in cases:
            with self.subTest(label):
                baseline = {"BM_Format_ChooseByteUnit": 0.7, "BM_Numeric_ToDouble_Int": 0.7, name: base_ns}
                current = {**baseline, name: cur_ns}
                code, output = self.run_gate(baseline, current, overrides=self.floor_override(3))
                self.assertEqual(code, expected, output)
                self.assertIn("BM_Format_ChooseByteUnit: floor 3.00ns", output)

    def test_a_floor_override_reports_its_own_floor(self):
        baseline = {"BM_Format_ChooseByteUnit": 0.7}
        code, output = self.run_gate(baseline, {"BM_Format_ChooseByteUnit": 3.1}, overrides=self.floor_override(3))
        self.assertEqual(code, 0, output)
        self.assertIn("within the 1.00ns noise floor (or their own floor) (1, not regressions)", output)
        self.assertIn("BM_Format_ChooseByteUnit: 0.70ns -> 3.10ns  (+342.9%)  [floor 3.00ns]", output)
        self.assertIn("40.0% threshold and the 1.00ns floor (1 with their own)", output)
        code, output = self.run_gate(baseline, {"BM_Format_ChooseByteUnit": 4.0}, overrides=self.floor_override(3))
        self.assertEqual(code, 1, output)
        self.assertIn("BM_Format_ChooseByteUnit: 0.7ns -> 4.0ns  (+471.4%)  [floor 3.00ns]", output)
        # A floor alone leaves the benchmark on --threshold.
        self.assertIn("regressed beyond 40.0%.", output)

    def test_a_floor_override_keeps_the_threshold(self):
        # 10ns -> 13.9ns is +3.9ns, over the 3ns floor, but +39%, under the 40% threshold it still uses.
        code, output = self.run_gate(
            {"BM_Format_ChooseByteUnit": 10.0}, {"BM_Format_ChooseByteUnit": 13.9}, overrides=self.floor_override(3)
        )
        self.assertEqual(code, 0, output)

    def test_an_override_can_set_a_threshold_and_a_floor(self):
        overrides = self.floor_override(3, threshold=60)
        code, output = self.run_gate(
            {"BM_Format_ChooseByteUnit": 10.0}, {"BM_Format_ChooseByteUnit": 15.5}, overrides=overrides
        )
        self.assertEqual(code, 0, output)  # +55%: under its own 60%
        self.assertIn("BM_Format_ChooseByteUnit: 60.0%, floor 3.00ns", output)
        code, output = self.run_gate(
            {"BM_Format_ChooseByteUnit": 10.0}, {"BM_Format_ChooseByteUnit": 17.0}, overrides=overrides
        )
        self.assertEqual(code, 1, output)  # +70% and +7ns: over both
        self.assertIn("[threshold 60.0%, floor 3.00ns]", output)

    def test_an_invalid_floor_override_is_a_usage_error(self):
        cases = {
            "nan floor": self.floor_override(float("nan")),
            "negative floor": self.floor_override(-1),
            "bool floor": self.floor_override(True),
            "string floor": self.floor_override("3"),
            "neither limit": {"thresholds": {"BM_Format_ChooseByteUnit": {"reason": "code layout"}}},
        }
        for label, overrides in cases.items():
            with self.subTest(label):
                code, output = self.run_gate({"BM_Memory": 100.0}, {"BM_Memory": 100.0}, overrides=overrides)
                self.assertEqual(code, 2, output)
                self.assertIn("invalid threshold overrides", output)


if __name__ == "__main__":
    unittest.main()
