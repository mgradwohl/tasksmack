#!/usr/bin/env bash
# bench.sh — Run TaskSmack benchmarks with consistent settings.
#
# Usage:
#   ./tools/bench.sh [preset] [-- <extra args>]
#
# preset defaults to 'benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json.
#
# Environment overrides (used by the script tests):
#   TASKSMACK_BENCH_BIN      benchmark binary (default build/<preset>/bin/TaskSmackBenchmarks)
#   TASKSMACK_BENCH_OUT_DIR  output directory (default perf-data/)
# The output file is the script's own: an extra --benchmark_out or --benchmark_out_format is
# refused, because the redaction below must find the file the benchmark wrote.
#
# Exits with the benchmark's own exit code if it fails or crashes (#1423). Partial output is still
# redacted (or deleted when it cannot be parsed), so it never keeps the host name, but it is not
# reported as usable; output that cannot be redacted after a successful exit is deleted too.
#
# Examples:
#   ./tools/bench.sh                          # benchmark preset, default flags
#   ./tools/bench.sh debug                    # debug preset (slower, useful for profiling)
#   ./tools/bench.sh -- --benchmark_filter=ProcessModel
#   ./tools/bench.sh benchmark -- --benchmark_filter=ProcessModel

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------- defaults ---------------------------------------------------------
PRESET="${1:-benchmark}"
if [[ "${PRESET}" == "--" ]]; then
    PRESET="benchmark"
    shift
elif [[ $# -gt 0 && "${1}" != "--" ]]; then
    shift
fi
# Drop optional "--" separator before extra args
if [[ $# -gt 0 && "${1}" == "--" ]]; then shift; fi

OUT_DIR="${TASKSMACK_BENCH_OUT_DIR:-${REPO_ROOT}/perf-data}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUT_FILE="${OUT_DIR}/${PRESET}-${TIMESTAMP}.json"

BENCH_BIN="${TASKSMACK_BENCH_BIN:-${REPO_ROOT}/build/${PRESET}/bin/TaskSmackBenchmarks}"

# ---------- sanity checks ----------------------------------------------------
if [[ ! -f "${BENCH_BIN}" ]]; then
    echo "Benchmark binary not found: ${BENCH_BIN}"
    echo "Build first:  cmake --build --preset ${PRESET}"
    exit 1
fi

# Google Benchmark takes the last --benchmark_out(_format), so an extra one would write somewhere
# the redaction never looks.
for arg in "$@"; do
    if [[ "${arg}" =~ ^--benchmark_out(_format)?(=|$) ]]; then
        echo "'${arg}' is not allowed: bench.sh sets the benchmark output file and format itself." >&2
        echo "Set TASKSMACK_BENCH_OUT_DIR to choose where results are written." >&2
        exit 2
    fi
done

mkdir -p "${OUT_DIR}"

# ---------- run ---------------------------------------------------------------
# Flags chosen for statistical consistency:
#   --benchmark_repetitions=10    → 10 independent runs per benchmark
#   --benchmark_min_time=0.5s     → minimum wall time before rep finishes
#   --benchmark_report_aggregates_only → emit mean/median/stddev, not raw reps
#   --benchmark_display_aggregates_only → clean terminal output
echo "Running benchmarks (preset=${PRESET}) → ${OUT_FILE}"
echo "Binary: ${BENCH_BIN}"
echo

# Capture the exit code instead of letting `set -e` abort here, so a failed run's partial output
# is still redacted (or deleted) before the script fails (#1423).
BENCH_EXIT=0
"${BENCH_BIN}" \
    --benchmark_repetitions=10 \
    --benchmark_min_time=0.5s \
    --benchmark_report_aggregates_only=true \
    --benchmark_display_aggregates_only=true \
    --benchmark_out="${OUT_FILE}" \
    --benchmark_out_format=json \
    "$@" || BENCH_EXIT=$?

# Redact machine-identifying context so results are safe to commit (repo convention:
# host_name "redacted", bare executable name).
# python3 is already a build prerequisite (GLAD loader generation).
redact_result() {
    python3 - "${OUT_FILE}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
data = json.loads(path.read_text(encoding="utf-8"))
context = data.setdefault("context", {})
context["host_name"] = "redacted"
context["executable"] = pathlib.PurePath(context.get("executable", "")).name or "TaskSmackBenchmarks"
path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
PY
}

if [[ ${BENCH_EXIT} -ne 0 ]]; then
    # A failed or crashed run may have left a partial JSON file holding the host name: redact it
    # if it parses, otherwise delete it, then fail with the benchmark's own exit code.
    if [[ -f "${OUT_FILE}" ]] && ! redact_result 2>/dev/null; then
        rm -f "${OUT_FILE}"
        echo "Deleted unparseable partial benchmark output '${OUT_FILE}' (it could not be redacted)." >&2
    fi
    echo "Benchmark binary exited with code ${BENCH_EXIT}; results in '${OUT_FILE}' are not usable." >&2
    exit "${BENCH_EXIT}"
fi

# Output that cannot be redacted (empty or truncated JSON despite exit code 0) may still hold the
# host name, so it is deleted rather than left behind, and the script fails.
if [[ ! -f "${OUT_FILE}" ]]; then
    echo "Benchmark output '${OUT_FILE}' not found; cannot redact machine-identifying context." >&2
    exit 1
fi
if ! redact_result; then
    rm -f "${OUT_FILE}"
    echo "Benchmark output '${OUT_FILE}' could not be redacted and was deleted." >&2
    exit 1
fi

echo
echo "Results written to: ${OUT_FILE}"
echo "Compare two runs with Google Benchmark's compare.py:"
echo "  python -m google_benchmark.compare perf-data/linux-baseline.json ${OUT_FILE}"
