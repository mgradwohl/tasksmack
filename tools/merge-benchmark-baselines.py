#!/usr/bin/env python3
"""
tools/merge-benchmark-baselines.py -- One CI benchmark baseline from captures on several runner classes (#3395).

Usage:
    merge-benchmark-baselines.py --output perf-data/linux-ci-baseline.json [LABEL=]CAPTURE.json ...

GitHub's hosted runners come in CPU classes that are not slower or faster across the board: on
ubuntu-26.04 a 2600 MHz class reads the thread-handoff benchmarks (*_Concurrent_PublicationWait)
about twice as slow as a 3240 MHz one, and the /proc and syscall benchmarks (BM_ProcessProbe_*,
BM_*Model_Refresh) two to three times as fast. A baseline captured on any one class then fails a
run on another. So the CI baseline takes each benchmark from the capture whose median for it is
the slowest: the gate's threshold applies against the slowest class seen for that benchmark, and a
run on any of the captured classes passes unless the code got slower.

Each capture is a Google Benchmark JSON file (tools/bench.sh, or the heavy-checks
benchmark-results artifact's perf-data/benchmark-current.json). For every benchmark (run_name) the
output holds every entry of the capture with the largest median real_time for it, so its median,
mean, stddev and cv stay consistent with each other. A benchmark missing, skipped or errored (any
entry with error_occurred) in a capture is taken from the others. The output's context is the first capture's, plus "tasksmack_merged_from":
each capture's label (LABEL=, such as the heavy-checks run id; the file name without one), CPU MHz
and how many benchmarks it supplied.

Exit codes: 0 written, 2 usage error or unreadable input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

TIME_UNIT_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def median_ns(entry: dict) -> float | None:
    """The entry's real time in nanoseconds when it is a measured median, else None."""
    if entry.get("aggregate_name") != "median" or entry.get("skipped") or entry.get("error_occurred"):
        return None
    scale = TIME_UNIT_NS.get(entry.get("time_unit", "ns"))
    time = entry.get("real_time")
    if scale is None or not isinstance(time, (int, float)):
        return None
    return float(time) * scale


def merge(captures: list[tuple[str, dict]]) -> dict:
    """The merged baseline of @p captures, each (name, parsed Google Benchmark JSON)."""
    slowest: dict[str, tuple[float, int]] = {}  # run_name -> (median ns, capture index)
    order: list[str] = []
    for index, (_, data) in enumerate(captures):
        entries = data.get("benchmarks", [])
        # A capture whose run of a benchmark errored in any entry can't supply it: the regression checker gives an
        # error precedence over the median, so copying that group would leave the benchmark uncomparable.
        errored = {e.get("run_name") or e.get("name") for e in entries if e.get("error_occurred")}
        for entry in entries:
            name = entry.get("run_name") or entry.get("name")
            if name not in slowest and name not in order:
                order.append(name)
            median = median_ns(entry)
            if median is not None and name not in errored and (name not in slowest or median > slowest[name][0]):
                slowest[name] = (median, index)

    supplied = [0] * len(captures)
    benchmarks: list[dict] = []
    for name in order:
        # A benchmark no capture measured (skipped everywhere, as the GPU probes on a runner) comes from the first
        # capture that lists it, so the gate still reports it as not measured.
        source = slowest[name][1] if name in slowest else next(
            i for i, (_, data) in enumerate(captures) if any((e.get("run_name") or e.get("name")) == name for e in data.get("benchmarks", []))
        )
        supplied[source] += 1
        benchmarks += [e for e in captures[source][1].get("benchmarks", []) if (e.get("run_name") or e.get("name")) == name]

    context = dict(captures[0][1].get("context", {}))
    context["tasksmack_merged_from"] = [
        {"capture": name, "mhz_per_cpu": data.get("context", {}).get("mhz_per_cpu"), "benchmarks": supplied[i]}
        for i, (name, data) in enumerate(captures)
    ]
    return {"context": context, "benchmarks": benchmarks}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("captures", nargs="+", help="[LABEL=]path of a Google Benchmark JSON capture")
    try:
        args = parser.parse_args(argv)
    except SystemExit:
        return 2
    captures: list[tuple[str, dict]] = []
    for argument in args.captures:
        label, separator, rest = argument.partition("=")
        path = Path(rest if separator else argument)
        try:
            captures.append((label if separator else path.name, json.loads(path.read_text(encoding="utf-8"))))
        except (OSError, json.JSONDecodeError) as error:
            print(f"error: {path}: {error}", file=sys.stderr)
            return 2
    merged = merge(captures)
    args.output.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
    for source in merged["context"]["tasksmack_merged_from"]:
        print(f"{source['capture']} ({source['mhz_per_cpu']} MHz): {source['benchmarks']} benchmarks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
