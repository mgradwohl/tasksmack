#!/usr/bin/env python3
"""
tools/bench-across-launches.py -- Re-measure flagged benchmarks across separate launches (#3476, #3448).

Usage:
    bench-across-launches.py plan --current FIRST.json --regressions REGRESSIONS.json \
        [--launches 4] [--budget-seconds 600]
    bench-across-launches.py combine --first FIRST.json --regressions REGRESSIONS.json \
        --output COMBINED.json [--baseline BASELINE.json] LAUNCH.json ...

On a shared hosted runner one launch of the benchmark binary can read a ns-scale benchmark 40-80%
slower than the next launch of the same binary: where the heap and the code land, and the noisy
neighbours of that launch, move it, and ten repetitions inside the launch all share that luck. So
the median of one launch's repetitions can't tell a regression from a slow launch, and each re-run
of one commit flagged a different set (#3476; #3448 on Windows). The maintainer's decision
(2026-11-05, #3448) is to measure across launches: run the noisy benchmarks in several separate
launches and compare the median across them. Not best-of-N, and no threshold changes.

plan: given the full run (FIRST.json) and the names tools/check-benchmark-regression.py
--regressions-out flagged, print one --benchmark_filter regex that matches exactly those benchmarks
(anchored, each name escaped), and refuse (exit 1) when re-running them --launches more times would
take longer than --budget-seconds. The estimate is the time the first launch spent in their
repetitions (iterations x real_time, which sums to the suite's wall time within a few percent) plus a
second per benchmark per launch for its setup. Over budget, the flag stands as a regression: that
many slowed benchmarks is not what a slow launch looks like, and the job has no time to tell.

combine: FIRST.json is launch 1; each LAUNCH.json is a further separate launch of the flagged
benchmarks alone. For each flagged benchmark, its real_time and cpu_time become the median of the
launches' medians (launch 1 included), and its other rows (the repetitions, mean, stddev, cv) are
dropped, so the file holds one median per benchmark; "tasksmack_launch_real_times" lists each
launch's median in the row's time_unit. Every other benchmark is FIRST.json's, unchanged, so the
regression check can re-run on COMBINED.json at the same thresholds and the same coverage. Each
flagged benchmark's launches are printed, against --baseline when it is given, so a real regression
(slow in every launch) reads as one at a glance. A flagged benchmark with no usable median in any
launch (missing, skipped, errored, or not a finite positive time) fails the combine (exit 2) rather
than letting the check pass on less data.

Exit codes: 0 done; 1 (plan) nothing to re-measure, or over budget; 2 usage error or unusable input.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

TIME_UNIT_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
# Google Benchmark compiles the filter as a POSIX extended regex (std::regex::extended): these are
# its metacharacters. Python's re.escape would also escape characters ERE gives no meaning to.
ERE_SPECIAL = set("\\.[]()*+?{}|^$")
SETUP_SECONDS_PER_BENCHMARK = 1.0


class InputError(Exception):
    """Unusable input: exit code 2."""


def run_name(entry: dict) -> str:
    return entry.get("run_name") or entry.get("name") or ""


def load_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise InputError(f"{path}: {error}") from error
    if not isinstance(data, dict):
        raise InputError(f"{path}: expected a JSON object")
    return data


def load_run(path: Path) -> dict:
    data = load_json(path)
    if not isinstance(data.get("benchmarks"), list) or not all(isinstance(e, dict) for e in data["benchmarks"]):
        raise InputError(f"{path}: expected a 'benchmarks' array of objects")
    return data


def load_regressions(path: Path) -> list[str]:
    data = load_json(path)
    names = data.get("regressions")
    if not isinstance(names, list) or not all(isinstance(n, str) and n for n in names):
        raise InputError(f"{path}: expected a 'regressions' array of benchmark names")
    return list(dict.fromkeys(names))


def ere_escape(name: str) -> str:
    return "".join(f"\\{c}" if c in ERE_SPECIAL else c for c in name)


def benchmark_filter(names: list[str]) -> str:
    """A --benchmark_filter that matches exactly @p names (Google Benchmark searches the full name)."""
    return "^(" + "|".join(ere_escape(n) for n in names) + ")$"


def to_ns(value: object, unit: object) -> float | None:
    """@p value in @p unit as nanoseconds, or None when it is not a finite, positive time."""
    scale = TIME_UNIT_NS.get(unit if isinstance(unit, str) else "")
    if scale is None or isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value) or value <= 0:
        return None
    return float(value) * scale


def measured_seconds(data: dict, name: str) -> float:
    """The time @p data's repetitions of @p name spent measuring: iterations x real_time."""
    total = 0.0
    for entry in data["benchmarks"]:
        if run_name(entry) != name or entry.get("run_type") != "iteration":
            continue
        iterations = entry.get("iterations")
        time = to_ns(entry.get("real_time"), entry.get("time_unit", "ns"))
        if isinstance(iterations, int) and not isinstance(iterations, bool) and iterations > 0 and time is not None:
            total += iterations * time / 1e9
    return total


def median_row(data: dict, name: str) -> dict | None:
    """@p name's median row in @p data, or None when it has none or any of its rows failed or skipped."""
    rows = [e for e in data["benchmarks"] if run_name(e) == name]
    if any(e.get("error_occurred") or e.get("skipped") for e in rows):
        return None
    medians = [e for e in rows if e.get("aggregate_name") == "median"]
    if len(medians) != 1 or to_ns(medians[0].get("real_time"), medians[0].get("time_unit", "ns")) is None:
        return None
    return medians[0]


def plan(args: argparse.Namespace) -> int:
    names = load_regressions(args.regressions)
    first = load_run(args.current)
    if not names:
        print("Nothing to re-measure: the check flagged no regression.", file=sys.stderr)
        return 1
    per_launch = sum(measured_seconds(first, n) + SETUP_SECONDS_PER_BENCHMARK for n in names)
    estimate = per_launch * args.launches
    print(
        f"{len(names)} flagged benchmark(s), about {per_launch:.0f}s a launch: {args.launches} more launch(es) "
        f"take about {estimate:.0f}s of the {args.budget_seconds:.0f}s budget.",
        file=sys.stderr,
    )
    if estimate > args.budget_seconds:
        print(
            f"Over budget: the {len(names)} flagged benchmark(s) are not re-measured, so the flags stand as "
            "regressions.",
            file=sys.stderr,
        )
        return 1
    print(benchmark_filter(names))
    return 0


def describe(ns: float, unit: str) -> str:
    return f"{ns / TIME_UNIT_NS[unit]:.2f}{unit}"


def combine(args: argparse.Namespace) -> int:
    names = load_regressions(args.regressions)
    first = load_run(args.first)
    launches = [first] + [load_run(path) for path in args.launches]
    baseline = load_run(args.baseline) if args.baseline is not None else None

    replaced: dict[str, dict] = {}
    unusable: list[str] = []
    print(f"Median across {len(launches)} separate launches, for the {len(names)} flagged benchmark(s):")
    for name in names:
        rows = [median_row(launch, name) for launch in launches]
        missing = [str(i + 1) for i, row in enumerate(rows) if row is None]
        if missing:
            unusable.append(f"{name}: no usable median in launch {', '.join(missing)}")
            continue
        row = dict(rows[0])
        unit = row.get("time_unit", "ns")
        real = [to_ns(r["real_time"], r.get("time_unit", "ns")) for r in rows]
        real_median = statistics.median(real)
        row["real_time"] = real_median / TIME_UNIT_NS[unit]
        cpu = [to_ns(r.get("cpu_time"), r.get("time_unit", "ns")) for r in rows]
        if all(c is not None for c in cpu):
            row["cpu_time"] = statistics.median(cpu) / TIME_UNIT_NS[unit]
        row["tasksmack_launch_real_times"] = [ns / TIME_UNIT_NS[unit] for ns in real]
        replaced[name] = row

        base_ns = None
        if baseline is not None:
            base_row = median_row(baseline, name)
            if base_row is not None:
                base_ns = to_ns(base_row["real_time"], base_row.get("time_unit", "ns"))

        def show(ns: float) -> str:
            text = describe(ns, unit)
            return f"{text} ({(ns - base_ns) / base_ns * 100:+.1f}%)" if base_ns else text

        print(f"   {name}:" + (f" baseline {describe(base_ns, unit)}" if base_ns else ""))
        for index, ns in enumerate(real):
            print(f"      launch {index + 1}: {show(ns)}")
        print(f"      median: {show(real_median)}")

    if unusable:
        for line in unusable:
            print(f"ERROR: {line}", file=sys.stderr)
        return 2

    benchmarks: list[dict] = []
    for entry in first["benchmarks"]:
        name = run_name(entry)
        if name not in replaced:
            benchmarks.append(entry)
        elif entry.get("aggregate_name") == "median":
            benchmarks.append(replaced[name])
    context = dict(first.get("context", {}))
    context["tasksmack_across_launches"] = {"launches": len(launches), "benchmarks": names}
    args.output.write_text(json.dumps({"context": context, "benchmarks": benchmarks}, indent=2) + "\n", encoding="utf-8")
    return 0


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError(f"{value!r} must be at least 1")
    return parsed


def budget(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError(f"{value!r} must be a finite number")
    return parsed


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    planner = sub.add_parser("plan", help="print the filter for the flagged benchmarks, within the budget")
    planner.add_argument("--current", required=True, type=Path, help="the full run that was checked")
    planner.add_argument("--regressions", required=True, type=Path, help="check-benchmark-regression.py --regressions-out")
    planner.add_argument("--launches", type=positive_int, default=4, help="further launches to budget for (default 4)")
    planner.add_argument("--budget-seconds", type=budget, default=600.0, help="time they may take (default 600)")
    combiner = sub.add_parser("combine", help="the median across launches of each flagged benchmark")
    combiner.add_argument("--first", required=True, type=Path, help="the full run that was checked (launch 1)")
    combiner.add_argument("--regressions", required=True, type=Path, help="check-benchmark-regression.py --regressions-out")
    combiner.add_argument("--output", required=True, type=Path)
    combiner.add_argument("--baseline", type=Path, default=None, help="print each launch against this baseline")
    combiner.add_argument("launches", nargs="+", type=Path, help="the further launches' JSON files")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return 0 if error.code == 0 else 2
    try:
        return plan(args) if args.command == "plan" else combine(args)
    except InputError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
