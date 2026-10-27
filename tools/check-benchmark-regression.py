#!/usr/bin/env python3
"""
tools/check-benchmark-regression.py — Compare benchmark results against a baseline.

Usage:
    python3 tools/check-benchmark-regression.py --baseline perf-data/linux-baseline.json \
        --current perf-data/benchmark-latest.json [--threshold 15] [--min-coverage 90] \
        [--min-abs-delta-ns 1.0] [--threshold-overrides perf-data/linux-ci-thresholds.json]

Exit codes:
    0  No matched benchmark exceeds both its threshold and its floor, and coverage meets --min-coverage
    1  One or more benchmarks regressed beyond the threshold, or coverage is too low
    2  Usage error, invalid input, or no comparable benchmarks at all

The threshold is a percentage: a benchmark that is more than THRESHOLD% slower than the
baseline is considered a regression. Improvements are always accepted.

--min-abs-delta-ns is an absolute noise floor: a benchmark only counts as a regression when it
is both more than THRESHOLD% slower AND slower by more than this many nanoseconds. A
sub-nanosecond microbenchmark (e.g. BM_Numeric_ToDouble_Int at ~0.4ns) moving by 0.2ns is timer
and runner noise, yet reads as +50%; percentages alone cannot tell that apart from a real
regression (#1322). Default 1.0ns; pass 0 to gate on the percentage alone.

--min-coverage is the minimum percentage of baseline benchmarks that must be present and
validly comparable in the current run. A benchmark missing from the current run, or one with
no usable timing data on either side, counts against coverage rather than being silently
ignored -- a regression-detection gate that quietly compares less and less over time as
benchmarks disappear or degrade is not trustworthy. Default 90% tolerates the occasional
benchmark with no usable timing without masking a real, larger coverage loss.

A benchmark that skipped itself on purpose (Google Benchmark's SkipWithMessage(), written as
"skipped": true) on either side is reported as not measured: neither compared nor counted in
coverage, in the numerator or the denominator. The GPU benchmarks do this on a machine with no
GPU, such as the CI runner, instead of timing an empty probe (#1420). A benchmark that failed
(SkipWithError(), "error_occurred": true) on either side still counts against coverage, even if
the other side skipped on purpose.

--threshold-overrides names a JSON file of per-benchmark limits that replace --threshold and/or
--min-abs-delta-ns for the benchmarks it lists (#2336, #3126). It is opt-in: without it every
benchmark uses the command-line values. The file is an object with a "thresholds" object mapping
each exact benchmark name (its run_name) to an object holding a non-empty "reason" and at least
one of "threshold" (a percentage, like --threshold) and "min_abs_delta_ns" (nanoseconds, like
--min-abs-delta-ns); a limit the entry leaves out keeps the command-line value:

    {"thresholds": {"BM_ProcessModel_GetSnapshots": {"threshold": 60, "reason": "allocator-bound"},
                    "BM_Format_ChooseByteUnit": {"min_abs_delta_ns": 3, "reason": "code layout"}}}

It is for the few benchmarks that move far more than the rest with no code change. The
allocator-bound full-snapshot copy depends on the hosted-runner class: on runners that are fast
at arithmetic but slow at memory it sits just under --threshold, so one global threshold either
fails at random on it or has to loosen for every benchmark. A sub-nanosecond benchmark can move
by a few cycles when an unrelated change shifts code layout: BM_Format_ChooseByteUnit measured
0.43-0.87ns for weeks, then 3.1ns and 2.9ns on two builds and 0.74ns on the next, with no change
to the code it times, while the other sub-nanosecond benchmarks stayed within 0.3ns (#3126). That
is +320%, and 2.4ns is over the 1.0ns floor, so only an absolute floor of its own fits: a
percentage would have to exceed 400%, and raising --min-abs-delta-ns to 3ns for every benchmark
would let a 2.6ns benchmark double unnoticed. With "min_abs_delta_ns": 3 it passes at 0.7ns ->
3.1ns (+2.4ns) and still fails at 0.7ns -> 4.0ns (+3.3ns) or 2ns -> 12ns, and every other
benchmark keeps the 1.0ns floor, so 10ns -> 40ns, or 0.7ns -> 3.1ns anywhere else, still fails.
An entry for a benchmark the baseline does not have is reported and otherwise ignored.

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

    # tools/bench.sh keeps every repetition (#1424): per-repetition rows share the median row's
    # run_name, so the median always wins over them regardless of row order. A benchmark run
    # without repetitions has no aggregates, and its single iteration row is used instead.
    # A failed measurement (errored()) on any row beats both, in either order: a median computed
    # from the repetitions that did succeed must not hide one that failed (#1445 review), so the
    # benchmark still counts against coverage.
    result: dict[str, dict] = {}
    medians: set[str] = set()
    errors: set[str] = set()
    for bm in data["benchmarks"]:
        if not isinstance(bm, dict):
            raise ValueError("each entry in 'benchmarks' must be a JSON object")
        aggregate_name = bm.get("aggregate_name")
        name = bm.get("run_name") or bm.get("name", "")
        if not name or name in errors:
            continue
        if errored(bm):
            result[name] = bm
            errors.add(name)
            continue
        if aggregate_name and aggregate_name != "median":
            continue
        if aggregate_name == "median":
            result[name] = bm
            medians.add(name)
        elif name not in medians:
            result[name] = bm
    return result


def skip_message(bm: dict) -> str | None:
    """The message of a benchmark that skipped itself on purpose (SkipWithMessage()), else None.

    Google Benchmark writes "skipped": true and "skip_message" for an intentional skip, and
    "error_occurred": true for SkipWithError(); an error is a failed measurement, not a skip.
    """
    if bm.get("skipped") is True and bm.get("error_occurred") is not True:
        message = bm.get("skip_message")
        return message if isinstance(message, str) and message else "skipped"
    return None


def errored(bm: dict) -> bool:
    """True for a benchmark whose measurement failed (SkipWithError(): "error_occurred": true)."""
    return bm.get("error_occurred") is True


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
    """True if value is a finite, non-negative number (excluding bool)."""
    # bool is a subclass of int in Python, so float(True) == 1.0 and float(False) == 0.0 would
    # otherwise pass this check silently -- a corrupted JSON input with a literal true/false
    # where a timing number belongs must not slip through as "1ns"/"0ns".
    if isinstance(value, bool):
        return False
    try:
        value = float(value)
    except (TypeError, ValueError):
        return False
    return math.isfinite(value) and value >= 0


OVERRIDE_LIMITS = ("threshold", "min_abs_delta_ns")


def load_threshold_overrides(path: Path) -> dict[str, dict[str, float]]:
    """Return the per-benchmark limits in a --threshold-overrides file: for each benchmark, the
    limits it sets, keyed "threshold" (percent) and/or "min_abs_delta_ns" (nanoseconds).

    Raises ValueError for anything but the documented shape: an entry that sets neither limit, a
    limit that is not a finite, non-negative number (a NaN would silently pass every comparison,
    as with --threshold), or an entry with no reason, since an override nobody can explain is one
    nobody can safely remove.
    """
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("thresholds"), dict):
        raise ValueError("expected a JSON object containing a 'thresholds' object")
    overrides: dict[str, dict[str, float]] = {}
    for name, entry in data["thresholds"].items():
        if not name:
            raise ValueError("an override has an empty benchmark name")
        if not isinstance(entry, dict):
            raise ValueError(f"{name}: expected an object with 'threshold' and/or 'min_abs_delta_ns' and 'reason'")
        if not any(key in entry for key in OVERRIDE_LIMITS):
            raise ValueError(f"{name}: an override must set 'threshold', 'min_abs_delta_ns', or both")
        limits: dict[str, float] = {}
        for key in OVERRIDE_LIMITS:
            if key not in entry:
                continue
            value = entry[key]
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value < 0
            ):
                raise ValueError(f"{name}: '{key}' must be a finite number >= 0, not {value!r}")
            limits[key] = float(value)
        reason = entry.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            raise ValueError(f"{name}: 'reason' must be a non-empty string")
        overrides[name] = limits
    return overrides


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
    parser.add_argument(
        "--min-abs-delta-ns",
        type=finite_percentage(min_value=0.0),
        default=1.0,
        help=(
            "Absolute noise floor in nanoseconds: a slowdown of this much or less is never a "
            "regression, whatever its percentage (default: 1.0; 0 disables)"
        ),
    )
    parser.add_argument(
        "--threshold-overrides",
        type=Path,
        default=None,
        help=(
            "JSON file of per-benchmark limits that replace --threshold and/or --min-abs-delta-ns "
            "for the benchmarks it names (default: none, every benchmark uses the flags' values)"
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

    overrides: dict[str, dict[str, float]] = {}
    if args.threshold_overrides is not None:
        if not args.threshold_overrides.exists():
            print(f"ERROR: threshold overrides file not found: {args.threshold_overrides}", file=sys.stderr)
            return 2
        try:
            overrides = load_threshold_overrides(args.threshold_overrides)
        except (OSError, json.JSONDecodeError, ValueError) as exc:
            print(f"ERROR: invalid threshold overrides: {exc}", file=sys.stderr)
            return 2

    own_thresholds = {name for name, limits in overrides.items() if "threshold" in limits}
    own_floors = {name for name, limits in overrides.items() if "min_abs_delta_ns" in limits}

    def threshold_for(name: str) -> float:
        return overrides.get(name, {}).get("threshold", args.threshold)

    def floor_for(name: str) -> float:
        return overrides.get(name, {}).get("min_abs_delta_ns", args.min_abs_delta_ns)

    def describe_limits(name: str) -> str:
        """The limits a benchmark's override sets, e.g. "60.0%" or "floor 3.00ns"."""
        limits = overrides.get(name, {})
        parts = []
        if "threshold" in limits:
            parts.append(f"{limits['threshold']:.1f}%")
        if "min_abs_delta_ns" in limits:
            parts.append(f"floor {limits['min_abs_delta_ns']:.2f}ns")
        return ", ".join(parts)

    def own_threshold_note(name: str) -> str:
        if name not in overrides:
            return ""
        limits = describe_limits(name)
        return f"  [threshold {limits}]" if name in own_thresholds else f"  [{limits}]"

    if not baseline:
        print("ERROR: baseline file contains no usable benchmarks.", file=sys.stderr)
        return 2
    if not current:
        print("ERROR: current file contains no usable benchmarks.", file=sys.stderr)
        return 2

    print(f"Comparing {len(current)} current benchmark(s) against {len(baseline)} baseline(s).")
    print(f"Regression threshold: {args.threshold:.1f}%")
    print(f"Absolute noise floor: {args.min_abs_delta_ns:.2f}ns")
    print(f"Minimum required coverage: {args.min_coverage:.1f}%")
    if overrides:
        print(f"Per-benchmark thresholds ({len(overrides)}, from {args.threshold_overrides}):")
        for name in sorted(overrides):
            note = "" if name in baseline else "  (not in baseline, ignored)"
            print(f"   {name}: {describe_limits(name)}{note}")
    print()

    # Skipped on purpose (e.g. a GPU benchmark on a machine with no GPU, #1420): there is nothing
    # to compare, and it is not a coverage loss either. Classified first, over every benchmark on
    # either side: a baseline record that skipped is not measured whether or not the current run
    # has it at all (otherwise it would land in missing_from_current and count against coverage),
    # and a skipped benchmark only the current run has is reported here too, not as new.
    # A failed measurement (SkipWithError(), "error_occurred": true) on either side takes
    # precedence: it is never classified as an intentional skip, so it still counts against
    # coverage -- a baseline skip must not hide a current-run error, nor the reverse.
    not_measured: list[tuple[str, str]] = []
    for name in sorted(set(baseline) | set(current)):
        if (name in baseline and errored(baseline[name])) or (name in current and errored(current[name])):
            continue
        base_skip = skip_message(baseline[name]) if name in baseline else None
        cur_skip = skip_message(current[name]) if name in current else None
        if cur_skip is not None:
            not_measured.append((name, f"skipped in current run: {cur_skip}"))
        elif base_skip is not None:
            not_measured.append((name, f"skipped in baseline run: {base_skip}"))
    skipped_names = {name for name, _ in not_measured}
    # Only baseline benchmarks are in the coverage denominator, so only they are taken out of it.
    skipped_baseline = len(skipped_names & set(baseline))

    # A benchmark present in the baseline but absent from the current run is a real coverage
    # gap, not something to silently ignore -- it means this run tells us nothing about
    # whether that benchmark regressed (see #871: "removing a required baseline benchmark
    # returned success").
    missing_from_current = sorted(set(baseline) - set(current) - skipped_names)
    new_in_current = sorted(set(current) - set(baseline) - skipped_names)

    regressions: list[tuple[str, float, str, float, str, float]] = []
    improvements: list[tuple[str, float, str, float, str, float]] = []
    below_floor: list[tuple[str, float, str, float, str, float]] = []
    invalid: list[tuple[str, str]] = []
    compared = 0

    for name in sorted((set(baseline) & set(current)) - skipped_names):
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
            base_time_normalized = normalize_time(base_time, base_unit)
        except ValueError as exc:
            invalid.append((name, f"baseline: {exc}"))
            continue
        try:
            cur_time_normalized = normalize_time(cur_time, cur_unit)
        except ValueError as exc:
            invalid.append((name, f"current: {exc}"))
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

        abs_delta_ns = cur_time_normalized - base_time_normalized

        if pct_change > threshold_for(name):
            row = (name, base_time, base_unit, cur_time, cur_unit, pct_change)
            if abs_delta_ns > floor_for(name):
                regressions.append(row)
            else:
                below_floor.append(row)
        elif pct_change < -5.0:
            improvements.append((name, base_time, base_unit, cur_time, cur_unit, pct_change))

    # ── Report coverage gaps ────────────────────────────────────────────────────
    if missing_from_current:
        print(f"Missing from current run ({len(missing_from_current)}) -- present in baseline "
              "but not measured this run:")
        for name in missing_from_current:
            print(f"   {name}")
        print()

    if not_measured:
        print(f"Not measured ({len(not_measured)}) -- skipped by the benchmark itself, "
              "excluded from comparison and coverage:")
        for name, reason in not_measured:
            print(f"   {name}: {reason}")
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

    # ── Report over-threshold changes inside the absolute noise floor ─────────
    if below_floor:
        own = " (or their own threshold)" if own_thresholds else ""
        own_floor = " (or their own floor)" if own_floors else ""
        print(f"Over {args.threshold:.1f}%{own} but within the "
              f"{args.min_abs_delta_ns:.2f}ns noise floor{own_floor} ({len(below_floor)}, not regressions):")
        for name, base, base_unit, cur, cur_unit, pct in sorted(below_floor, key=lambda x: -x[5]):
            print(f"   {name}: {base:.2f}{base_unit} -> {cur:.2f}{cur_unit}  ({pct:+.1f}%){own_threshold_note(name)}")
        print()

    # ── Report regressions ────────────────────────────────────────────────────
    if regressions:
        print(f"Regressions ({len(regressions)}) - exceeded {args.threshold:.1f}% threshold:")
        for name, base, base_unit, cur, cur_unit, pct in sorted(regressions, key=lambda x: -x[5]):
            print(f"   {name}: {base:.1f}{base_unit} -> {cur:.1f}{cur_unit}  ({pct:+.1f}%){own_threshold_note(name)}")
        print()
        override_note = " (or their own threshold)" if any(name in own_thresholds for name, *_ in regressions) else ""
        print(f"FAILED: {len(regressions)} benchmark(s) regressed beyond {args.threshold:.1f}%{override_note}.")
        return 1

    # ── Coverage gate ─────────────────────────────────────────────────────────
    # Benchmarks skipped on purpose are out of the denominator as well as the numerator.
    measurable = len(baseline) - skipped_baseline
    coverage_pct = (compared / measurable) * 100.0
    if coverage_pct < args.min_coverage:
        print(
            f"FAILED: coverage {coverage_pct:.1f}% is below the required "
            f"{args.min_coverage:.1f}% ({len(missing_from_current)} missing, "
            f"{len(invalid)} invalid/unusable, out of {measurable} measurable baseline benchmark(s); "
            f"{skipped_baseline} skipped on purpose)."
        )
        return 1

    floor_note = f" ({len(below_floor)} over {args.threshold:.1f}% only within the noise floor)" if below_floor else ""
    skip_note = f" ({skipped_baseline} skipped on purpose, not counted)" if skipped_baseline else ""
    override_note = f" ({len(own_thresholds)} with their own)" if own_thresholds else ""
    own_floor_note = f" ({len(own_floors)} with their own)" if own_floors else ""
    print(f"No benchmark exceeded both the {args.threshold:.1f}% threshold{override_note} and the "
          f"{args.min_abs_delta_ns:.2f}ns floor{own_floor_note}: {compared} compared{floor_note}; "
          f"coverage {coverage_pct:.1f}%{skip_note}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
