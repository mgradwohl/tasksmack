#!/usr/bin/env python3
"""
tools/check-benchmark-regression.py — Compare benchmark results against a baseline.

Usage:
    python3 tools/check-benchmark-regression.py --baseline perf-data/linux-baseline.json \
        --current perf-data/benchmark-latest.json [--threshold 15] [--min-coverage 90]

Exit codes:
    0  All matched benchmarks are within threshold, and coverage meets --min-coverage
    1  One or more benchmarks regressed beyond the threshold, or coverage is too low
    2  Usage error, invalid input, or no comparable benchmarks at all

The threshold is a percentage: a benchmark that is more than THRESHOLD% slower than the
baseline is considered a regression. Improvements are always accepted.

--min-coverage is the minimum percentage of baseline benchmarks that must be present and
validly comparable in the current run. A benchmark missing from the current run, or one with
no usable timing data on either side, counts against coverage rather than being silently
ignored -- a regression-detection gate that quietly compares less and less over time as
benchmarks disappear or degrade is not trustworthy. Default 90% tolerates the occasional
counter-only benchmark that structurally has no timing field (e.g. BM_GPUModel_UtilizationHistory)
without masking a real, larger coverage loss.

The JSON format is Google Benchmark's --benchmark_format=json output.
"""

import argparse
import json
import math
import sys
from pathlib import Path


UNIT_TO_NANOSECONDS = {
    "ns": 1.0,
    "us": 1000.0,
    "ms": 1000_000.0,
    "s": 1000_000_000.0,
}

def normalize_time(value: float, unit: str) -> float:
    """Convert a benchmark time value into nanoseconds for comparison."""
    factor = UNIT_TO_NANOSECONDS.get(unit)
    if factor is None:
        raise ValueError(f"unsupported time unit: {unit}")
    return value * factor


def load_benchmarks(path: Path) -> dict[str, dict]:
    """Return a dict mapping benchmark name → benchmark record."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("benchmarks"), list):
        raise ValueError("expected a JSON object containing a 'benchmarks' array")

    result: dict[str, dict] = {}
    for bm in data["benchmarks"]:
        if not isinstance(bm, dict):
            raise ValueError("each entry in 'benchmarks' must be a JSON object")
        aggregate_name = bm.get("aggregate_name")
        if aggregate_name and aggregate_name != "median":
            continue
        name = bm.get("run_name") or bm.get("name", "")
        if name:
            result[name] = bm
    return result


def common_timing_field(base_bm: dict, cur_bm: dict) -> str | None:
    """Return the shared timing field to compare, preferring real_time.

    cpu_time is only used as a fallback when real_time is absent from *both* records. If
    real_time is present on one side but missing on the other, that's a genuine data mismatch
    between the two records (e.g. different Google Benchmark reporting settings) -- not a case
    where quietly comparing cpu_time on both sides instead would be a safe substitute, since the
    side that *does* have real_time was never validated against cpu_time for equivalence. No
    comparison is made in that case rather than silently downgrading to a lesser-preferred field
    only one side actually needed to fall back to.
    """
    if "real_time" in base_bm and "real_time" in cur_bm:
        return "real_time"
    if "real_time" not in base_bm and "real_time" not in cur_bm:
        if "cpu_time" in base_bm and "cpu_time" in cur_bm:
            return "cpu_time"
    return None


def is_valid_time(value: object) -> bool:
    """True if value is a finite, non-negative number."""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return False
    return math.isfinite(value) and value >= 0


def finite_percentage(min_value: float, max_value: float | None = None):
    """argparse type factory: a finite float within [min_value, max_value].

    Plain `type=float` accepts "nan"/"inf"/"-inf" from the command line. Since every gate in
    this script is a `>`/`<` comparison against these values, a NaN threshold or coverage floor
    would make every such comparison silently evaluate False -- bypassing the regression and
    coverage gates entirely rather than erroring. Rejecting non-finite/out-of-range values here
    makes that a usage error (exit 2, via argparse's own ArgumentTypeError handling) instead.
    """

    def convert(value: str) -> float:
        try:
            parsed = float(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"{value!r} is not a valid number") from exc
        if not math.isfinite(parsed):
            raise argparse.ArgumentTypeError(f"{value!r} must be a finite number")
        if parsed < min_value:
            raise argparse.ArgumentTypeError(f"{value!r} must be >= {min_value}")
        if max_value is not None and parsed > max_value:
            raise argparse.ArgumentTypeError(f"{value!r} must be <= {max_value}")
        return parsed

    return convert


def main() -> int:
    parser = argparse.ArgumentParser(description="Check for benchmark regressions.")
    parser.add_argument("--baseline", required=True, type=Path, help="Baseline JSON file")
    parser.add_argument("--current",  required=True, type=Path, help="Current run JSON file")
    parser.add_argument(
        "--threshold",
        type=finite_percentage(min_value=0.0),
        default=15.0,
        help="Regression threshold in percent (default: 15)",
    )
    parser.add_argument(
        "--min-coverage",
        type=finite_percentage(min_value=0.0, max_value=100.0),
        default=90.0,
        help=(
            "Minimum percent of baseline benchmarks that must be present and validly "
            "comparable in the current run (default: 90)"
        ),
    )
    args = parser.parse_args()

    if not args.baseline.exists():
        print(f"ERROR: baseline file not found: {args.baseline}", file=sys.stderr)
        return 2
    if not args.current.exists():
        print(f"ERROR: current file not found: {args.current}", file=sys.stderr)
        return 2

    try:
        baseline = load_benchmarks(args.baseline)
        current = load_benchmarks(args.current)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"ERROR: invalid benchmark input: {exc}", file=sys.stderr)
        return 2

    if not baseline:
        print("ERROR: baseline file contains no usable benchmarks.", file=sys.stderr)
        return 2
    if not current:
        print("ERROR: current file contains no usable benchmarks.", file=sys.stderr)
        return 2

    print(f"Comparing {len(current)} current benchmark(s) against {len(baseline)} baseline(s).")
    print(f"Regression threshold: {args.threshold:.1f}%")
    print(f"Minimum required coverage: {args.min_coverage:.1f}%")
    print()

    # A benchmark present in the baseline but absent from the current run is a real coverage
    # gap, not something to silently ignore -- it means this run tells us nothing about
    # whether that benchmark regressed (see #871: "removing a required baseline benchmark
    # returned success").
    missing_from_current = sorted(set(baseline) - set(current))
    new_in_current = sorted(set(current) - set(baseline))

    regressions: list[tuple[str, float, str, float, str, float]] = []
    improvements: list[tuple[str, float, str, float, str, float]] = []
    invalid: list[tuple[str, str]] = []
    compared = 0

    for name in sorted(set(baseline) & set(current)):
        base_bm = baseline[name]
        cur_bm = current[name]

        field = common_timing_field(base_bm, cur_bm)
        if field is None:
            invalid.append((name, "no common timing field (real_time/cpu_time) present on both sides"))
            continue

        base_time_raw = base_bm.get(field)
        cur_time_raw = cur_bm.get(field)

        if not is_valid_time(base_time_raw):
            invalid.append((name, f"baseline {field} is missing, non-finite, or negative: {base_time_raw!r}"))
            continue
        if not is_valid_time(cur_time_raw):
            invalid.append((name, f"current {field} is missing, non-finite, or negative: {cur_time_raw!r}"))
            continue

        base_time = float(base_time_raw)
        cur_time = float(cur_time_raw)
        base_unit = base_bm.get("time_unit", "ns")
        cur_unit = cur_bm.get("time_unit", "ns")

        try:
            cur_time_normalized = normalize_time(cur_time, cur_unit)
            base_time_normalized = normalize_time(base_time, base_unit)
        except ValueError as exc:
            invalid.append((name, str(exc)))
            continue

        if base_time_normalized == 0:
            invalid.append((name, "baseline timing normalized to zero"))
            continue
        if cur_time_normalized == 0:
            # A genuine 0ns/0us/etc. measurement is implausible for anything Google Benchmark's
            # own timer resolution can report -- almost certainly a broken/skipped measurement,
            # not a real result. Without this, a 0 current timing against a non-zero baseline
            # computes a "-100% improvement" and counts as successfully compared, which can mask
            # exactly the kind of measurement failure --min-coverage exists to catch.
            invalid.append((name, "current timing normalized to zero"))
            continue

        compared += 1
        pct_change = ((cur_time_normalized - base_time_normalized) / base_time_normalized) * 100.0

        if pct_change > args.threshold:
            regressions.append((name, base_time, base_unit, cur_time, cur_unit, pct_change))
        elif pct_change < -5.0:
            improvements.append((name, base_time, base_unit, cur_time, cur_unit, pct_change))

    # ── Report coverage gaps ────────────────────────────────────────────────────
    if missing_from_current:
        print(f"Missing from current run ({len(missing_from_current)}) -- present in baseline "
              "but not measured this run:")
        for name in missing_from_current:
            print(f"   {name}")
        print()

    if invalid:
        print(f"Invalid/unusable comparisons ({len(invalid)}):")
        for name, reason in invalid:
            print(f"  SKIP  {name}: {reason}")
        print()

    if new_in_current:
        print(f"New in current run ({len(new_in_current)}, not in baseline, no comparison possible):")
        for name in new_in_current:
            print(f"   {name}")
        print()

    if compared == 0:
        print("ERROR: no valid matching benchmarks could be compared.", file=sys.stderr)
        return 2

    # ── Report improvements ───────────────────────────────────────────────────
    if improvements:
        print(f"Improvements ({len(improvements)}):")
        for name, base, base_unit, cur, cur_unit, pct in sorted(improvements, key=lambda x: x[5]):
            print(f"   {name}: {base:.1f}{base_unit} -> {cur:.1f}{cur_unit}  ({pct:+.1f}%)")
        print()

    # ── Report regressions ────────────────────────────────────────────────────
    if regressions:
        print(f"Regressions ({len(regressions)}) - exceeded {args.threshold:.1f}% threshold:")
        for name, base, base_unit, cur, cur_unit, pct in sorted(regressions, key=lambda x: -x[5]):
            print(f"   {name}: {base:.1f}{base_unit} -> {cur:.1f}{cur_unit}  ({pct:+.1f}%)")
        print()
        print(f"FAILED: {len(regressions)} benchmark(s) regressed beyond {args.threshold:.1f}%.")
        return 1

    # ── Coverage gate ─────────────────────────────────────────────────────────
    coverage_pct = (compared / len(baseline)) * 100.0
    if coverage_pct < args.min_coverage:
        print(
            f"FAILED: coverage {coverage_pct:.1f}% is below the required "
            f"{args.min_coverage:.1f}% ({len(missing_from_current)} missing, "
            f"{len(invalid)} invalid/unusable, out of {len(baseline)} baseline benchmark(s))."
        )
        return 1

    print(f"All {compared} matched benchmark(s) within {args.threshold:.1f}% threshold; "
          f"coverage {coverage_pct:.1f}%.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
