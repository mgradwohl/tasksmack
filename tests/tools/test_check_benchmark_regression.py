#!/usr/bin/env python3
"""Tests for tools/check-benchmark-regression.py's regression gate (#1322 noise floor, #1420 skips)."""

import json
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
    ):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            current_path = write_run(tmp_path, "current.json", current)
            if current_extra:
                add_records(current_path, current_extra)
            baseline_path = write_run(tmp_path, "baseline.json", baseline)
            if baseline_extra:
                add_records(baseline_path, baseline_extra)
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
                text=True,
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
                    text=True,
                    check=False,
                )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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


if __name__ == "__main__":
    unittest.main()
