#!/usr/bin/env python3
"""Tests for tools/merge-benchmark-baselines.py: each benchmark from the capture whose median is the slowest (#3395)."""

import contextlib
import importlib.util
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "merge-benchmark-baselines.py"
spec = importlib.util.spec_from_file_location("merge_benchmark_baselines", SCRIPT)
merge_benchmark_baselines = importlib.util.module_from_spec(spec)
spec.loader.exec_module(merge_benchmark_baselines)


def capture(mhz: int, timings: dict[str, tuple[float, str]], skipped: tuple[str, ...] = ()) -> dict:
    """A capture with a median and a mean per benchmark (real_time, time_unit), and skipped ones."""
    benchmarks = []
    for name, (time, unit) in timings.items():
        for aggregate, scale in (("mean", 1.01), ("median", 1.0)):
            benchmarks.append(
                {"name": f"{name}_{aggregate}", "run_name": name, "aggregate_name": aggregate, "real_time": time * scale, "time_unit": unit}
            )
    for name in skipped:
        benchmarks.append({"name": name, "run_name": name, "skipped": True})
    return {"context": {"mhz_per_cpu": mhz, "host_name": f"runner-{mhz}"}, "benchmarks": benchmarks}


class MergeBenchmarkBaselinesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="merge-benchmark-baselines-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def test_each_benchmark_comes_whole_from_the_capture_slowest_at_it(self) -> None:
        # The 2600 MHz class is slower at thread handoff and faster at /proc, as on the hosted runners.
        slow_handoff = capture(2600, {"BM_Handoff": (0.2, "us"), "BM_Proc": (2.0, "ms")})
        slow_proc = capture(3240, {"BM_Handoff": (100.0, "ns"), "BM_Proc": (6.0, "ms")})
        merged = merge_benchmark_baselines.merge([("a", slow_handoff), ("b", slow_proc)])
        by_name = {(e["run_name"], e["aggregate_name"]): e for e in merged["benchmarks"]}
        self.assertEqual(by_name[("BM_Handoff", "median")]["real_time"], 0.2)
        self.assertEqual(by_name[("BM_Handoff", "median")]["time_unit"], "us", "units compared in ns, entries kept as captured")
        self.assertAlmostEqual(by_name[("BM_Handoff", "mean")]["real_time"], 0.202, msg="its mean from the same capture")
        self.assertEqual(by_name[("BM_Proc", "median")]["real_time"], 6.0)
        self.assertEqual(merged["context"]["host_name"], "runner-2600", "the first capture's context")
        self.assertEqual(
            merged["context"]["tasksmack_merged_from"],
            [{"capture": "a", "mhz_per_cpu": 2600, "benchmarks": 1}, {"capture": "b", "mhz_per_cpu": 3240, "benchmarks": 1}],
        )

    def test_a_benchmark_skipped_or_missing_in_one_capture_comes_from_another(self) -> None:
        first = capture(2600, {"BM_A": (5.0, "ns")}, skipped=("BM_B",))
        second = capture(3240, {"BM_A": (4.0, "ns"), "BM_B": (7.0, "ns"), "BM_C": (9.0, "ns")})
        merged = merge_benchmark_baselines.merge([("first", first), ("second", second)])
        medians = {e["run_name"]: e["real_time"] for e in merged["benchmarks"] if e.get("aggregate_name") == "median"}
        self.assertEqual(medians, {"BM_A": 5.0, "BM_B": 7.0, "BM_C": 9.0})
        self.assertFalse(any(e.get("skipped") for e in merged["benchmarks"]), "the measured one, not the skip")

    def test_a_capture_whose_run_errored_never_supplies_that_benchmark(self) -> None:
        # The slower capture's median is 200 ns, but one of its entries errored: the clean 100 ns capture supplies it.
        errored = capture(2600, {"BM_A": (200.0, "ns"), "BM_B": (3.0, "ns")})
        errored["benchmarks"].append({"name": "BM_A", "run_name": "BM_A", "error_occurred": True, "error_message": "boom"})
        clean = capture(3240, {"BM_A": (100.0, "ns"), "BM_B": (2.0, "ns")})
        merged = merge_benchmark_baselines.merge([("errored", errored), ("clean", clean)])
        bm_a = [e for e in merged["benchmarks"] if e["run_name"] == "BM_A"]
        self.assertFalse(any(e.get("error_occurred") for e in bm_a), "no error row copied into the baseline")
        self.assertEqual([e["real_time"] for e in bm_a if e.get("aggregate_name") == "median"], [100.0])
        medians = {e["run_name"]: e["real_time"] for e in merged["benchmarks"] if e.get("aggregate_name") == "median"}
        self.assertEqual(medians["BM_B"], 3.0, "the errored capture still supplies the benchmarks it ran cleanly")

    def test_a_non_finite_or_non_positive_median_never_displaces_a_valid_one(self) -> None:
        # Seen first, a NaN would stick (every comparison with it is false); an infinity would win every one.
        for bad in (float("nan"), float("inf"), 0.0, -5.0):
            with self.subTest(bad=bad):
                broken = capture(2600, {"BM_A": (bad, "ns")})
                valid = capture(3240, {"BM_A": (100.0, "ns")})
                merged = merge_benchmark_baselines.merge([("broken", broken), ("valid", valid)])
                medians = [e["real_time"] for e in merged["benchmarks"] if e.get("aggregate_name") == "median"]
                self.assertEqual(medians, [100.0])

    def test_a_benchmark_no_capture_measured_stays_skipped(self) -> None:
        merged = merge_benchmark_baselines.merge([("only", capture(2600, {"BM_A": (1.0, "ns")}, skipped=("BM_GPU",)))])
        gpu = [e for e in merged["benchmarks"] if e["run_name"] == "BM_GPU"]
        self.assertEqual(len(gpu), 1)
        self.assertTrue(gpu[0]["skipped"])

    def test_main_writes_the_merge_with_labels_and_rejects_unreadable_input(self) -> None:
        a = self.root / "a.json"
        b = self.root / "b.json"
        a.write_text(json.dumps(capture(2600, {"BM_A": (2.0, "ns")})), encoding="utf-8")
        b.write_text(json.dumps(capture(3240, {"BM_A": (3.0, "ns")})), encoding="utf-8")
        out = self.root / "baseline.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, merge_benchmark_baselines.main(["--output", str(out), f"run-1={a}", str(b)]))
        merged = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual([s["capture"] for s in merged["context"]["tasksmack_merged_from"]], ["run-1", "b.json"])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, merge_benchmark_baselines.main(["--output", str(out), str(self.root / "missing.json")]))
            self.assertEqual(2, merge_benchmark_baselines.main([]))


if __name__ == "__main__":
    unittest.main()
