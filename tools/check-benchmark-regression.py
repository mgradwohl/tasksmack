#!/usr/bin/env python3
"""
tools/check-benchmark-regression.py — Compare benchmark results against a baseline.

Usage:
    python3 tools/check-benchmark-regression.py --baseline perf-data/linux-baseline.json \
        --current perf-data/benchmark-latest.json [--threshold 15]

Exit codes:
    0  All benchmarks within threshold (or no matching benchmarks found)
    1  One or more benchmarks regressed beyond the threshold
    2  Usage error / file not found

The threshold is a percentage: a benchmark that is more than THRESHOLD% slower than the
baseline is considered a regression.  Improvements are always accepted.

The JSON format is Google Benchmark's --benchmark_format=json output.
"""

import argparse
import json
import sys
from pathlib import Path


def load_benchmarks(path: Path) -> dict[str, dict]:
    """Return a dict mapping benchmark name → benchmark record."""
    data = json.loads(path.read_text())
    result: dict[str, dict] = {}
    for bm in data.get("benchmarks", []):
        name = bm.get("name", "")
        # Skip aggregate rows (mean, median, stddev, cv)
        if bm.get("aggregate_name") in ("mean", "median", "stddev", "cv"):
            continue
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

    regressions: list[tuple[str, float, float, float]] = []
    improvements: list[tuple[str, float, float, float]] = []
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

        if cur_time is None or base_time is None or base_time == 0:
            print(f"  SKIP  {name}: missing timing data")
            continue

        pct_change = ((cur_time - base_time) / base_time) * 100.0

        if pct_change > args.threshold:
            regressions.append((name, base_time, cur_time, pct_change))
        elif pct_change < -5.0:
            improvements.append((name, base_time, cur_time, pct_change))

    if matched == 0:
        print("No matching benchmark names between baseline and current run.")
        print("Run will be treated as passing (no regressions detectable).")
        return 0

    # ── Report improvements ───────────────────────────────────────────────────
    if improvements:
        print(f"✅ Improvements ({len(improvements)}):")
        for name, base, cur, pct in sorted(improvements, key=lambda x: x[3]):
            unit = "ns"
            print(f"   {name}: {base:.1f}{unit} → {cur:.1f}{unit}  ({pct:+.1f}%)")
        print()

    # ── Report regressions ────────────────────────────────────────────────────
    if regressions:
        print(f"❌ Regressions ({len(regressions)}) — exceeded {args.threshold:.1f}% threshold:")
        for name, base, cur, pct in sorted(regressions, key=lambda x: -x[3]):
            unit = "ns"
            print(f"   {name}: {base:.1f}{unit} → {cur:.1f}{unit}  ({pct:+.1f}%)")
        print()
        print(f"FAILED: {len(regressions)} benchmark(s) regressed beyond {args.threshold:.1f}%.")
        return 1

    print(f"✅ All {matched} matched benchmark(s) within {args.threshold:.1f}% threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
