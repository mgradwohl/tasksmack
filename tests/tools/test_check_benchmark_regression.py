#!/usr/bin/env python3
"""Tests for tools/check-benchmark-regression.py's regression gate (#1322 noise floor)."""

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


class CheckBenchmarkRegressionTest(unittest.TestCase):
    def run_gate(self, baseline: dict[str, float], current: dict[str, float], *extra: str):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--baseline",
                    str(write_run(tmp_path, "baseline.json", baseline)),
                    "--current",
                    str(write_run(tmp_path, "current.json", current)),
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


if __name__ == "__main__":
    unittest.main()
