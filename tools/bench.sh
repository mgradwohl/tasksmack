#!/usr/bin/env bash
# bench.sh — Run TaskSmack benchmarks with consistent settings.
#
# Usage:
#   ./tools/bench.sh [preset] [-- <extra args>]
#
# preset defaults to 'benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json, holding every repetition plus the
# mean/median/stddev/cv aggregates, and a provenance sidecar at
# perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config, benchmark
# args, anonymized machine class; written by tools/bench-manifest.py, see CONTRIBUTING.md
# "Benchmark Output").
#
# Environment overrides (used by the script tests):
#   TASKSMACK_BENCH_BIN      benchmark binary (default build/<preset>/bin/TaskSmackBenchmarks)
#   TASKSMACK_BENCH_OUT_DIR  output directory (default perf-data/)
#
# Exits non-zero if the benchmark binary fails or crashes; any partial output is still redacted
# (or deleted when it cannot be) and the manifest records the exit code.
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
MANIFEST_FILE="${OUT_DIR}/${PRESET}-${TIMESTAMP}.manifest.json"

BENCH_BIN="${TASKSMACK_BENCH_BIN:-${REPO_ROOT}/build/${PRESET}/bin/TaskSmackBenchmarks}"

# ---------- sanity checks ----------------------------------------------------
if [[ ! -f "${BENCH_BIN}" ]]; then
    echo "Benchmark binary not found: ${BENCH_BIN}"
    echo "Build first:  cmake --build --preset ${PRESET}"
    exit 1
fi

mkdir -p "${OUT_DIR}"

# ---------- run ---------------------------------------------------------------
# Flags chosen for statistical consistency:
#   --benchmark_repetitions=10    → 10 independent runs per benchmark
#   --benchmark_min_time=0.5s     → minimum wall time before rep finishes
#   --benchmark_display_aggregates_only → clean terminal output
# --benchmark_report_aggregates_only is deliberately NOT passed: the JSON file keeps every
# repetition (so the distribution can be re-analysed) plus the mean/median/stddev/cv aggregate
# rows that tools/check-benchmark-regression.py compares.
BENCH_ARGS=(
    --benchmark_repetitions=10
    --benchmark_min_time=0.5s
    --benchmark_display_aggregates_only=true
    --benchmark_out="${OUT_FILE}"
    --benchmark_out_format=json
    "$@"
)

echo "Running benchmarks (preset=${PRESET}) → ${OUT_FILE}"
echo "Binary: ${BENCH_BIN}"
echo

# Capture the exit code instead of letting `set -e` abort here, so a failed run's partial output
# is still redacted (or deleted) before the script fails (#1423).
BENCH_EXIT=0
"${BENCH_BIN}" "${BENCH_ARGS[@]}" || BENCH_EXIT=$?

# The manifest is written before the result is redacted, so the result file stays the newest one
# in the output directory (heavy-checks.yml picks the latest perf-data/benchmark-*.json).
# python3 is already a build prerequisite (GLAD loader generation). A manifest failure is held
# until the result has been redacted, then fails the script.
MANIFEST_EXIT=0
python3 "${SCRIPT_DIR}/bench-manifest.py" \
    --manifest "${MANIFEST_FILE}" \
    --result "${OUT_FILE}" \
    --binary "${BENCH_BIN}" \
    --preset "${PRESET}" \
    --exit-code "${BENCH_EXIT}" \
    --repo-root "${REPO_ROOT}" \
    -- "${BENCH_ARGS[@]}" || MANIFEST_EXIT=$?

# Redact machine-identifying context so results are safe to commit (repo convention:
# host_name "redacted", bare executable name).
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
    echo "Benchmark binary exited with code ${BENCH_EXIT}; results in '${OUT_FILE}' are not usable. Manifest: ${MANIFEST_FILE}" >&2
    exit "${BENCH_EXIT}"
fi

# Fails closed under `set -e`: if redaction fails, the script aborts before reporting the results
# as ready to use.
redact_result

if [[ ${MANIFEST_EXIT} -ne 0 ]]; then
    echo "Writing the provenance manifest '${MANIFEST_FILE}' failed (exit ${MANIFEST_EXIT})." >&2
    exit "${MANIFEST_EXIT}"
fi

echo
echo "Results written to: ${OUT_FILE}"
echo "Provenance manifest: ${MANIFEST_FILE}"
echo "Compare two runs with Google Benchmark's compare.py:"
echo "  python -m google_benchmark.compare perf-data/linux-baseline.json ${OUT_FILE}"
