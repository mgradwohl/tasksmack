#!/usr/bin/env bash
# bench.sh — Run TaskSmack benchmarks with consistent settings.
#
# Usage:
#   ./tools/bench.sh [preset] [-- <extra args>]
#
# preset defaults to 'optimized' (best for perf measurements).
# Produces JSON output at perf-data/<preset>-<timestamp>.json.
#
# Examples:
#   ./tools/bench.sh                          # optimized preset, default flags
#   ./tools/bench.sh debug                    # debug preset (slower, useful for profiling)
#   ./tools/bench.sh -- --benchmark_filter=ProcessModel
#   ./tools/bench.sh optimized -- --benchmark_filter=ProcessModel

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------- defaults ---------------------------------------------------------
PRESET="${1:-optimized}"
if [[ "${PRESET}" == "--" ]]; then
    PRESET="optimized"
    shift
elif [[ $# -gt 0 && "${1}" != "--" ]]; then
    shift
fi
# Drop optional "--" separator before extra args
if [[ $# -gt 0 && "${1}" == "--" ]]; then shift; fi

OUT_DIR="${REPO_ROOT}/perf-data"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUT_FILE="${OUT_DIR}/${PRESET}-${TIMESTAMP}.json"

BENCH_BIN="${REPO_ROOT}/build/${PRESET}/benchmarks/tasksmack_benchmarks"

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
#   --benchmark_report_aggregates_only → emit mean/median/stddev, not raw reps
#   --benchmark_display_aggregates_only → clean terminal output
echo "Running benchmarks (preset=${PRESET}) → ${OUT_FILE}"
echo "Binary: ${BENCH_BIN}"
echo

"${BENCH_BIN}" \
    --benchmark_repetitions=10 \
    --benchmark_min_time=0.5s \
    --benchmark_report_aggregates_only=true \
    --benchmark_display_aggregates_only=true \
    --benchmark_out="${OUT_FILE}" \
    --benchmark_out_format=json \
    "$@"

echo
echo "Results written to: ${OUT_FILE}"
echo "Compare two runs with Google Benchmark's compare.py:"
echo "  python3 ./build/${PRESET}/_deps/benchmark-src/tools/compare.py benchmarks <baseline.json> <${PRESET}-${TIMESTAMP}.json>"
