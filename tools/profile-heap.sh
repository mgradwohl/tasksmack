#!/usr/bin/env bash
# tools/profile-heap.sh — Heap allocation profiling for TaskSmack using heaptrack.
#
# Usage:
#   ./tools/profile-heap.sh app   [--preset <preset>] [--skip-build]
#   ./tools/profile-heap.sh bench [--preset <preset>] [--skip-build]
#                                 [--bench-filter <regex>] [--bench-reps <n>]
#                                 [--bench-min-time <t>]
#
# Modes:
#   app   — launch TaskSmack under heaptrack, wait for exit. Default preset: profile.
#   bench — run TaskSmackBenchmarks under heaptrack. Default preset: benchmark.
#
# Outputs under perf-data/:
#   heaptrack-<mode>-<timestamp>.gz    — heaptrack data (open with heaptrack_gui or heaptrack_print)
#   heaptrack-<mode>-<timestamp>.log   — session log
#   heaptrack-<mode>-<timestamp>-summary.txt  — top allocators (if heaptrack_print available)
#
# Why heaptrack?
#   perf records CPU samples; heaptrack records every heap allocation and its call stack.
#   Use it to find: hot-path std::string/vector allocations, unexpected per-frame heap
#   activity, and peak memory consumers. ~2-3x runtime slowdown vs. Valgrind's ~50x.
#
# Requirements:
#   heaptrack          — sudo apt install heaptrack
#   heaptrack_print    — ships with heaptrack; headless/CI analysis
#   heaptrack_gui      — sudo apt install heaptrack  (optional Qt GUI)
#
# Examples:
#   ./tools/profile-heap.sh app
#   ./tools/profile-heap.sh bench --bench-filter 'BM_ProcessModel_Refresh$'
#   ./tools/profile-heap.sh bench --preset profile  # debug-info build for better attribution

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

PERF_DIR="${REPO_ROOT}/perf-data"

# ── argument parsing ──────────────────────────────────────────────────────────
MODE="${1:-}"
shift || true

if [[ -z "${MODE}" || ( "${MODE}" != "app" && "${MODE}" != "bench" ) ]]; then
    echo "Usage: $0 app|bench [--preset <preset>] [--skip-build] [--bench-filter <regex>] [--bench-reps <n>] [--bench-min-time <t>]" >&2
    exit 1
fi

PRESET=""
SKIP_BUILD=0
BENCH_FILTER='BM_(ProcessProbe_Enumerate|ProcessModel_Refresh|SystemProbe_Sample|SystemModel_Refresh|GPUProbe_ReadCounters|GPUModel_Refresh)$'
BENCH_REPS=3
BENCH_MIN_TIME="0.5s"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)         PRESET="$2";           shift 2 ;;
        --skip-build)     SKIP_BUILD=1;           shift   ;;
        --bench-filter)   BENCH_FILTER="$2";     shift 2 ;;
        --bench-reps)     BENCH_REPS="$2";       shift 2 ;;
        --bench-min-time) BENCH_MIN_TIME="$2";   shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${PRESET}" ]]; then
    PRESET=$([ "${MODE}" = "app" ] && echo "profile" || echo "benchmark")
fi

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
PREFIX="heaptrack-${MODE}"
# heaptrack appends its own suffix; we give it a base name
HEAPTRACK_BASE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}"
LOG_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}.log"
SUMMARY_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}-summary.txt"

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

print_step() {
    echo
    echo "──────────────────────────────────────────"
    echo "  $*"
    echo "──────────────────────────────────────────"
}

# ── pre-flight ────────────────────────────────────────────────────────────────
print_step "Checking prerequisites"

check_command heaptrack "apt install heaptrack" || \
    die "heaptrack is required. Install: sudo apt install heaptrack"

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    validate_build_prereqs || die "Build prerequisites not met."
fi

mkdir -p "${PERF_DIR}"

# ── build ─────────────────────────────────────────────────────────────────────
if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    print_step "Building preset=${PRESET}"
    cmake --preset "${PRESET}"
    cmake --build --preset "${PRESET}"
fi

# ── select binary ─────────────────────────────────────────────────────────────
if [[ "${MODE}" = "app" ]]; then
    BINARY="${REPO_ROOT}/build/${PRESET}/bin/TaskSmack"
else
    BINARY="${REPO_ROOT}/build/${PRESET}/bin/TaskSmackBenchmarks"
fi

[[ -x "${BINARY}" ]] || die "Binary not found or not executable: ${BINARY}. Build with: cmake --build --preset ${PRESET}"

# ── capture ───────────────────────────────────────────────────────────────────
print_step "Starting heaptrack capture (mode=${MODE}, preset=${PRESET})"
info "Output base: ${HEAPTRACK_BASE}"
info "Binary:      ${BINARY}"

{
    echo "HEAP_MODE=${MODE}"
    echo "PRESET=${PRESET}"
    echo "BINARY=${BINARY}"
    echo "HEAPTRACK_BASE=${HEAPTRACK_BASE}"
    echo "TIMESTAMP=${TIMESTAMP}"
} > "${LOG_FILE}"

if [[ "${MODE}" = "app" ]]; then
    echo ""
    echo "Launching TaskSmack under heaptrack. Exercise the application, then close it."
    echo ""
    heaptrack -o "${HEAPTRACK_BASE}" "${BINARY}" 2> >(tee -a "${LOG_FILE}" >&2)
else
    heaptrack -o "${HEAPTRACK_BASE}" "${BINARY}" \
        "--benchmark_filter=${BENCH_FILTER}" \
        "--benchmark_repetitions=${BENCH_REPS}" \
        "--benchmark_min_time=${BENCH_MIN_TIME}" \
        --benchmark_report_aggregates_only=true \
        --benchmark_display_aggregates_only=true 2> >(tee -a "${LOG_FILE}" >&2)
fi

# heaptrack writes <base>.heaptrack.gz or <base>.<pid>.gz — find it
HEAPTRACK_DATA="$(find "${PERF_DIR}" -maxdepth 1 -name "${PREFIX}-${TIMESTAMP}*.gz" -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2- || true)"

if [[ -z "${HEAPTRACK_DATA}" ]]; then
    die "heaptrack did not produce a .gz output file under ${PERF_DIR}. Check ${LOG_FILE}."
fi

echo "HEAPTRACK_DATA=${HEAPTRACK_DATA}" >> "${LOG_FILE}"

# ── headless analysis ────────────────────────────────────────────────────────
print_step "Analyzing allocation data"

if command -v heaptrack_print &>/dev/null; then
    heaptrack_print "${HEAPTRACK_DATA}" > "${SUMMARY_FILE}" 2>&1 || true
    echo ""
    echo "Top allocators:"
    # heaptrack_print output has sections; show the peak/leaked summary + top callstacks
    head -n 60 "${SUMMARY_FILE}"
    info "Full report: ${SUMMARY_FILE}"
else
    echo "NOTE: heaptrack_print not found — skipping headless analysis."
    echo "  Open the data in heaptrack_gui instead, or install:"
    echo "    sudo apt install heaptrack"
fi

# ── done ──────────────────────────────────────────────────────────────────────
print_step "Capture complete"
echo ""
echo "HEAPTRACK_DATA=${HEAPTRACK_DATA}"
echo "LOG=${LOG_FILE}"
if [[ -f "${SUMMARY_FILE}" ]]; then
    echo "SUMMARY=${SUMMARY_FILE}"
fi
echo ""
echo "Next steps:"
if command -v heaptrack_gui &>/dev/null; then
    echo "  heaptrack_gui ${HEAPTRACK_DATA}    # interactive GUI"
fi
if command -v heaptrack_print &>/dev/null; then
    echo "  heaptrack_print ${HEAPTRACK_DATA}  # detailed text report"
fi
echo "  # Install GUI: sudo apt install heaptrack"
