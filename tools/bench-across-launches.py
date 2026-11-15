#!/usr/bin/env python3
"""
tools/bench-across-launches.py -- Re-measure flagged benchmarks across separate launches (#3476, #3448).

Usage:
    bench-across-launches.py plan --current FIRST.json --regressions REGRESSIONS.json \
        [--launches 4] [--budget-seconds 600]
    bench-across-launches.py combine --first FIRST.json --regressions REGRESSIONS.json \
        --output COMBINED.json [--baseline BASELINE.json] [--exclude-first] LAUNCH.json ...
    bench-across-launches.py run --current FIRST.json --regressions REGRESSIONS.json \
        --baseline BASELINE.json --output COMBINED.json --work-dir DIR [--first JUDGED.json] \
        [--exclude-first] [--first-manifest FIRST.manifest.json] [--launches 4] [--budget-seconds 600] \
        [--report REPORT.txt] -- COMMAND ...

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
second per benchmark per launch for its setup. A /manual_time benchmark reports only the time it
chose to measure, not what it ran: measureConcurrentPublicationWait() (benchmarks/bench_ModelLimits.cpp)
does 11 handoffs per iteration and reports their median, and on CI those benchmarks report about
4 ms of repetitions against 1.1-1.3 s of wall time each (#3508). So a manual-time benchmark counts
25x its reported time plus 5 s of setup. The estimate is a first gate only: heavy-checks.yml also
stops the launches at a hard deadline. Over budget, the flag stands as a regression: that many
slowed benchmarks is not what a slow launch looks like, and the job has no time to tell.

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

combine --exclude-first (#3623): the launches ran on another machine than FIRST.json, the second runner
heavy-checks.yml hands the flags to when they stand on the first. Some hosted runners are a slower CPU
than the ones the baseline was measured on (Intel Xeon hosts at 2300 and 2800 MHz, against the AMD EPYC
ones), and there every launch is slow alike, so the first runner's own re-measurement can't tell the
host from the code. The flagged benchmarks' medians then come from the LAUNCH.json files alone, so the
second runner's verdict is its own; FIRST.json still supplies every other benchmark, for the same
coverage, and the output's context is the first LAUNCH.json's: the machine that measured them.

run (#3830, #3912): plan, the further launches, and combine, in one place for heavy-checks.yml's two benchmark
jobs and bench.sh --baseline. The plan is made from FIRST.json; each launch runs COMMAND with
--benchmark_filter=<the plan's filter> appended and TASKSMACK_BENCH_OUT_DIR set to DIR/<launch number>, as
bench.sh reads it, and its result is the file its last RESULT= line names. The launches are numbered 2 to
--launches + 1 after the full run, or 1 to --launches with --exclude-first, where none of them is the full
run. Every launch shares one deadline, --budget-seconds from the start: a launch that would start past it
isn't started, and one still running at it is stopped (its whole process group, SIGTERM and then SIGKILL 10 s
later) and its directory deleted, since a killed run leaves an unredacted partial result. Either way, as when
the plan refuses, the flags stand (exit 1). With --first-manifest, a launch whose manifest
(<result>.manifest.json) records another placement.core_class than it fails (exit 2), as bench.ps1 -Baseline
checks (#3163): a run pinned elsewhere can't share a median. The combine then reads --first (FIRST.json when
not given) and writes COMBINED.json; the regression check on it is the caller's, with the caller's
thresholds. Each launch's output goes to the console; the plan's estimate, any out-of-time notice, the
combine's table and every error also go to --report when it is given, for a job summary.

Exit codes: 0 done; 1 (plan) nothing to re-measure, or over budget; (run) the same, or out of time, so the
flags stand; 2 usage error or unusable input; 3 (run) a launch failed or wrote no result.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import TextIO

TIME_UNIT_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
# Google Benchmark compiles the filter as a POSIX extended regex (std::regex::extended): these are
# its metacharacters. Python's re.escape would also escape characters ERE gives no meaning to.
ERE_SPECIAL = set("\\.[]()*+?{}|^$")
SETUP_SECONDS_PER_BENCHMARK = 1.0
# A /manual_time benchmark (UseManualTime(), which Google Benchmark writes into its name) reports less
# than it runs; see the docstring.
MANUAL_TIME_FACTOR = 25.0
MANUAL_TIME_SETUP_SECONDS = 5.0
# run: how long a stopped launch gets between SIGTERM and SIGKILL, as heavy-checks.yml's timeout --kill-after.
KILL_AFTER_SECONDS = 10.0
# run's clock, a seam for the tests: the deadline is measured on it.
clock = time.monotonic


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


def is_manual_time(name: str) -> bool:
    return "/manual_time" in name


def launch_seconds(data: dict, name: str) -> float:
    """The estimated wall time of one launch of @p name, from its repetitions in @p data."""
    if is_manual_time(name):
        return measured_seconds(data, name) * MANUAL_TIME_FACTOR + MANUAL_TIME_SETUP_SECONDS
    return measured_seconds(data, name) + SETUP_SECONDS_PER_BENCHMARK


def median_row(data: dict, name: str) -> dict | None:
    """@p name's median row in @p data, or None when it has none or any of its rows failed or skipped."""
    rows = [e for e in data["benchmarks"] if run_name(e) == name]
    if any(e.get("error_occurred") or e.get("skipped") for e in rows):
        return None
    medians = [e for e in rows if e.get("aggregate_name") == "median"]
    if len(medians) != 1 or to_ns(medians[0].get("real_time"), medians[0].get("time_unit", "ns")) is None:
        return None
    return medians[0]


def planned_filter(names: list[str], first: dict, launches: int, budget_seconds: float, out: TextIO) -> str | None:
    """The filter for re-running @p names @p launches more times within @p budget_seconds, or None when there is
    nothing to re-measure or it would take longer. Says which, and the estimate, on @p out."""
    if not names:
        print("Nothing to re-measure: the check flagged no regression.", file=out)
        return None
    per_launch = sum(launch_seconds(first, n) for n in names)
    estimate = per_launch * launches
    print(
        f"{len(names)} flagged benchmark(s), about {per_launch:.0f}s a launch: {launches} more launch(es) "
        f"take about {estimate:.0f}s of the {budget_seconds:.0f}s budget.",
        file=out,
    )
    if estimate > budget_seconds:
        print(
            f"Over budget: the {len(names)} flagged benchmark(s) are not re-measured, so the flags stand as "
            "regressions.",
            file=out,
        )
        return None
    return benchmark_filter(names)


def plan(args: argparse.Namespace) -> int:
    names = load_regressions(args.regressions)
    first = load_run(args.current)
    pattern = planned_filter(names, first, args.launches, args.budget_seconds, sys.stderr)
    if pattern is None:
        return 1
    print(pattern)
    return 0


def describe(ns: float, unit: str) -> str:
    return f"{ns / TIME_UNIT_NS[unit]:.2f}{unit}"


def combine(args: argparse.Namespace) -> int:
    names = load_regressions(args.regressions)
    first = load_run(args.first)
    others = [load_run(path) for path in args.launches]
    launches = others if args.exclude_first else [first] + others
    baseline = load_run(args.baseline) if args.baseline is not None else None

    replaced: dict[str, dict] = {}
    unusable: list[str] = []
    where = " on this runner (the first runner's values not counted)" if args.exclude_first else ""
    print(f"Median across {len(launches)} separate launches{where}, for the {len(names)} flagged benchmark(s):")
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
    context = dict((launches[0] if args.exclude_first else first).get("context", {}))
    context["tasksmack_across_launches"] = {"launches": len(launches), "benchmarks": names}
    if args.exclude_first:
        context["tasksmack_across_launches"]["first_counted"] = False
    args.output.write_text(json.dumps({"context": context, "benchmarks": benchmarks}, indent=2) + "\n", encoding="utf-8")
    return 0


class Tee:
    """Writes to every stream it holds: the console and the report."""

    def __init__(self, *streams: TextIO) -> None:
        self.streams = streams

    def write(self, text: str) -> int:
        for stream in self.streams:
            stream.write(text)
        return len(text)

    def flush(self) -> None:
        for stream in self.streams:
            stream.flush()


def stop(process: subprocess.Popen) -> None:
    """Stops @p process and, on POSIX, every process in its group (bench.sh and the benchmark binary it runs)."""
    if os.name == "nt":
        process.kill()
        process.wait()
        return
    for sig, wait in ((signal.SIGTERM, KILL_AFTER_SECONDS), (signal.SIGKILL, None)):
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, sig)
        try:
            process.wait(timeout=wait)
            return
        except subprocess.TimeoutExpired:
            continue


def launch_once(command: list[str], out_dir: Path, seconds: float, console: TextIO) -> tuple[int | None, str | None]:
    """Runs @p command for at most @p seconds with TASKSMACK_BENCH_OUT_DIR=@p out_dir, its output on @p console.
    Returns its exit code and the path its last RESULT= line named, or (None, None) when it was stopped."""
    env = {**os.environ, "TASKSMACK_BENCH_OUT_DIR": str(out_dir)}
    # A session of its own, so a stop reaches the whole group (POSIX only; Windows ignores it).
    group = {"start_new_session": True} if os.name != "nt" else {}
    try:
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, encoding="utf-8",
            errors="replace", bufsize=1, **group)
    except OSError as error:
        print(f"ERROR: cannot start {command[0]}: {error}", file=sys.stderr)
        return 127, None
    results: list[str] = []

    def pump() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            console.write(line)
            console.flush()
            if line.startswith("RESULT="):
                results.append(line[len("RESULT="):].rstrip("\r\n"))

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    try:
        code = process.wait(timeout=max(seconds, 0.0))
    except subprocess.TimeoutExpired:
        stop(process)
        reader.join(timeout=5)
        return None, None
    reader.join(timeout=30)
    return code, (results[-1] if results else None)


def core_class(manifest: Path) -> str:
    """The placement.core_class a bench.sh manifest records, "(none)" without one, "(no manifest)" when unreadable."""
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "(no manifest)"
    placement = data.get("placement") if isinstance(data, dict) else None
    value = placement.get("core_class") if isinstance(placement, dict) else None
    return "(none)" if value is None else str(value)


def run(args: argparse.Namespace, console: TextIO) -> int:
    deadline = clock() + args.budget_seconds
    budget_text = f"{args.budget_seconds:g}"
    names = load_regressions(args.regressions)
    first = load_run(args.current)
    pattern = planned_filter(names, first, args.launches, args.budget_seconds, sys.stdout)
    if pattern is None:
        return 1

    def out_of_time(what: str) -> int:
        print(f"Out of time: {what}, past the {budget_text}s budget. The flags stand as regressions.")
        return 1

    first_class = core_class(args.first_manifest) if args.first_manifest is not None else None
    total = args.launches if args.exclude_first else args.launches + 1
    grouped = os.environ.get("GITHUB_ACTIONS") == "true"
    results: list[Path] = []
    for number in range(total - args.launches + 1, total + 1):
        remaining = deadline - clock()
        if remaining <= 0:
            return out_of_time(f"launch {number} of {total} not started")
        directory = args.work_dir / str(number)
        directory.mkdir(parents=True, exist_ok=True)
        print(f"::group::Launch {number} of {total}" if grouped else f"\nLaunch {number} of {total}: --benchmark_filter={pattern}",
              file=console, flush=True)
        code, result = launch_once([*args.launch_command, f"--benchmark_filter={pattern}"], directory, remaining, console)
        if grouped:
            print("::endgroup::", file=console, flush=True)
        if code is None:
            shutil.rmtree(directory, ignore_errors=True)
            return out_of_time(f"launch {number} of {total} stopped after {remaining:.0f}s")
        if code != 0:
            print(f"ERROR: launch {number} failed (exit {code})", file=sys.stderr)
            return 3
        if not result or not Path(result).is_file():
            print(f"ERROR: launch {number}'s output JSON not found (it printed RESULT={result or ''})", file=sys.stderr)
            return 3
        if first_class is not None:
            launch_class = core_class(Path(result).with_suffix(".manifest.json"))
            if launch_class != first_class:
                print(f"ERROR: launch {number} ran on core class {launch_class}, not the full run's {first_class}, so "
                      "its times can't share a median with the full run's. Nothing is combined.", file=sys.stderr)
                return 2
        results.append(Path(result))

    # A flagged benchmark with no usable median in some launch fails here (exit 2), not quietly.
    return combine(argparse.Namespace(
        regressions=args.regressions, first=args.first or args.current, launches=results, baseline=args.baseline,
        output=args.output, exclude_first=args.exclude_first))


def run_reported(args: argparse.Namespace) -> int:
    """run, with what it says (not the launches' own output) also appended to --report."""
    console = sys.stdout

    def guarded() -> int:
        try:
            return run(args, console)
        except InputError as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 2

    if args.report is None:
        return guarded()
    with open(args.report, "a", encoding="utf-8") as report, \
            contextlib.redirect_stdout(Tee(sys.stdout, report)), contextlib.redirect_stderr(Tee(sys.stderr, report)):
        return guarded()


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
    combiner.add_argument(
        "--exclude-first", action="store_true",
        help="the launches ran on another runner: take the flagged medians from them alone (#3623)")
    combiner.add_argument("launches", nargs="+", type=Path, help="the further launches' JSON files")
    runner = sub.add_parser("run", help="plan, the further launches and combine (#3830)")
    runner.add_argument("--current", required=True, type=Path, help="the full run the plan estimates from")
    runner.add_argument("--regressions", required=True, type=Path, help="check-benchmark-regression.py --regressions-out")
    runner.add_argument("--baseline", required=True, type=Path, help="print each launch against this baseline")
    runner.add_argument("--output", required=True, type=Path, help="the combined run")
    runner.add_argument("--work-dir", required=True, type=Path, help="each launch's results go to <work-dir>/<number>")
    runner.add_argument("--first", type=Path, default=None, help="the run the combine takes the others from (--current)")
    runner.add_argument("--exclude-first", action="store_true", help="the launches ran on another runner (#3623)")
    runner.add_argument("--first-manifest", type=Path, default=None, help="every launch must share its core class")
    runner.add_argument("--launches", type=positive_int, default=4, help="further launches (default 4)")
    runner.add_argument("--budget-seconds", type=budget, default=600.0, help="time they may take (default 600)")
    runner.add_argument("--report", type=Path, default=None, help="also append what run says here")
    # Everything after the first "--" is the launch command, its own "--" included.
    command: list[str] = []
    if argv[:1] == ["run"]:
        if "--" not in argv:
            print("ERROR: run needs the launch command after --", file=sys.stderr)
            return 2
        split = argv.index("--")
        argv, command = argv[:split], argv[split + 1:]
        if not command:
            print("ERROR: run needs the launch command after --", file=sys.stderr)
            return 2
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return 0 if error.code == 0 else 2
    try:
        if args.command == "run":
            args.launch_command = command
            return run_reported(args)
        return plan(args) if args.command == "plan" else combine(args)
    except InputError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
