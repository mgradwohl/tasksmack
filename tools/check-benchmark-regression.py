#!/usr/bin/env python3
"""
tools/check-benchmark-regression.py — Compare benchmark results against a baseline.

Usage:
    python3 tools/check-benchmark-regression.py --baseline perf-data/linux-baseline.json \
        --current perf-data/benchmark-latest.json [--threshold 15]

Exit codes:
    0  All matched benchmarks are within threshold
    1  One or more benchmarks regressed beyond the threshold
    2  Usage error, invalid input, or no matching benchmarks

The threshold is a percentage: a benchmark that is more than THRESHOLD% slower than the
baseline is considered a regression.  Improvements are always accepted.

The JSON format is Google Benchmark's --benchmark_format=json output.
"""

import argparse
import json
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
    data = json.loads(path.read_text())
    result: dict[str, dict] = {}
    for bm in data.get("benchmarks", []):
        aggregate_name = bm.get("aggregate_name")
        if aggregate_name and aggregate_name != "median":
            continue
        name = bm.get("run_name") or bm.get("name", "")
        if name:
            result[name] = bm
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Check for benchmark regressions.")
    parser.add_argument("--baseline", required=True, type=Path, help="Baseline JSON file")
    parser.add_argument("--current",  required=True, type=Path, help="Current run JSON file")
    parser.add_argument(
        "--threshold",
        type=float,
        default=15.0,
        help="Regression threshold in percent (default: 15)",
    )
    args = parser.parse_args()

    if not args.baseline.exists():
        print(f"ERROR: baseline file not found: {args.baseline}", file=sys.stderr)
        return 2
    if not args.current.exists():
        print(f"ERROR: current file not found: {args.current}", file=sys.stderr)
        return 2

    baseline = load_benchmarks(args.baseline)
    current  = load_benchmarks(args.current)

    if not baseline:
        print("WARNING: baseline file contains no benchmarks; skipping comparison.")
        return 0

    print(f"Comparing {len(current)} current benchmark(s) against {len(baseline)} baseline(s).")
    print(f"Regression threshold: {args.threshold:.1f}%")
    print()

    regressions: list[tuple[str, float, str, float, str, float]] = []
    improvements: list[tuple[str, float, str, float, str, float]] = []
    matched = 0

    for name, cur_bm in current.items():
        if name not in baseline:
            # New benchmark — no comparison possible
            continue
        matched += 1
        base_bm = baseline[name]

        # Prefer real_time, fall back to cpu_time
        cur_time  = cur_bm.get("real_time")  or cur_bm.get("cpu_time")
        base_time = base_bm.get("real_time") or base_bm.get("cpu_time")
        cur_unit = cur_bm.get("time_unit", "ns")
        base_unit = base_bm.get("time_unit", "ns")

        if cur_time is None or base_time is None or base_time == 0:
            print(f"  SKIP  {name}: missing timing data")
            continue

        try:
            cur_time_normalized = normalize_time(cur_time, cur_unit)
            base_time_normalized = normalize_time(base_time, base_unit)
        except ValueError as exc:
            print(f"  SKIP  {name}: {exc}")
            continue

        if base_time_normalized == 0:
            print(f"  SKIP  {name}: baseline timing normalized to zero")
            continue

        pct_change = ((cur_time_normalized - base_time_normalized) / base_time_normalized) * 100.0

        if pct_change > args.threshold:
            regressions.append((name, base_time, base_unit, cur_time, cur_unit, pct_change))
        elif pct_change < -5.0:
            improvements.append((name, base_time, base_unit, cur_time, cur_unit, pct_change))

    if matched == 0:
        print("ERROR: no matching benchmark names between baseline and current run.", file=sys.stderr)
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

    print(f"All {matched} matched benchmark(s) within {args.threshold:.1f}% threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
