#!/usr/bin/env bash
# tools/profile-heap.sh — Heap allocation profiling for TaskSmack using heaptrack.
#
# Usage:
#   ./tools/profile-heap.sh app   [--preset <preset>] [--skip-build] [--user-config]
#   ./tools/profile-heap.sh bench [--preset <preset>] [--skip-build]
#                                 [--bench-filter <regex>] [--bench-reps <n>]
#                                 [--bench-min-time <t>]
#   ./tools/profile-heap.sh -h | --help   # print this help
#
# Modes:
#   app   — launch TaskSmack under heaptrack, wait for exit. Default preset: profile.
#   bench — run TaskSmackBenchmarks under heaptrack. Default preset: benchmark.
#
# App mode runs TaskSmack with a fresh, empty TASKSMACK_CONFIG_DIR, removed afterwards (#2250), so it
# starts from default settings, cannot rewrite yours, and is not stopped by the single-instance lock
# of a TaskSmack you already have open. --user-config runs it with the config it would use on its own
# instead, and refuses to start while a TaskSmack of yours is running. A TaskSmack turned away by
# another instance's lock (it logs "TaskSmack is already running", or exits with code 75) fails the
# run. TaskSmack's stdout is also copied to heaptrack-app-<timestamp>-app.log.
#
# Outputs under perf-data/:
#   heaptrack-<mode>-<timestamp>.gz|.zst — heaptrack data (open with heaptrack_gui or heaptrack_print)
#   heaptrack-<mode>-<timestamp>.log   — session log
#   heaptrack-app-<timestamp>-app.log  — TaskSmack's own stdout (app mode)
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
# -h / --help prints the header above without starting anything (#2276), as bench.sh does.
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,/^$/{/^$/d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"
    exit 0
fi

MODE="${1:-}"
shift || true

if [[ -z "${MODE}" || ( "${MODE}" != "app" && "${MODE}" != "bench" ) ]]; then
    echo "Usage: $0 app   [--preset <preset>] [--skip-build] [--user-config]" >&2
    echo "       $0 bench [--preset <preset>] [--skip-build] [--bench-filter <regex>] [--bench-reps <n>] [--bench-min-time <t>]" >&2
    echo "       $0 -h | --help" >&2
    exit 1
fi

PRESET=""
SKIP_BUILD=0
USE_USER_CONFIG=0
BENCH_FILTER='BM_(ProcessProbe_Enumerate|ProcessModel_Refresh|SystemProbe_Sample|SystemModel_Refresh|GPUProbe_ReadCounters|GPUModel_Refresh)$'
BENCH_REPS=3
BENCH_MIN_TIME="0.5s"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)         [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; PRESET="$2";           shift 2 ;;
        --skip-build)     SKIP_BUILD=1;           shift   ;;
        --bench-filter)   [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_FILTER="$2";     shift 2 ;;
        --bench-reps)     [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_REPS="$2";       shift 2 ;;
        --bench-min-time) [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_MIN_TIME="$2";   shift 2 ;;
        --user-config)    USE_USER_CONFIG=1;      shift   ;;
        -h|--help)        sed -n '2,/^$/{/^$/d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ "${MODE}" != "app" && "${USE_USER_CONFIG}" -eq 1 ]]; then
    echo "ERROR: --user-config applies to app mode only" >&2
    exit 1
fi

if [[ -z "${PRESET}" ]]; then
    PRESET=$([ "${MODE}" = "app" ] && echo "profile" || echo "benchmark")
fi

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
PREFIX="heaptrack-${MODE}"
# heaptrack appends its own suffix; we give it a base name
HEAPTRACK_BASE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}"
LOG_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}.log"
SUMMARY_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}-summary.txt"
APP_LOG="${PERF_DIR}/${PREFIX}-${TIMESTAMP}-app.log"

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

# The single-instance lock (#1230, #2250): a TaskSmack started while another holds its config
# directory's lock logs this, shows an "already running" box, and exits with this code
# (App::ALREADY_RUNNING_EXIT_CODE in src/App/InstanceLock.h).
ALREADY_RUNNING_MARKER="TaskSmack is already running"
ALREADY_RUNNING_EXIT_CODE=75

# The capture's own TASKSMACK_CONFIG_DIR (#2250), removed on exit; empty with --user-config.
CONFIG_DIR=""
# shellcheck disable=SC2317,SC2329 # invoked from EXIT traps
remove_config_dir() {
    if [[ -n "${CONFIG_DIR}" ]]; then
        rm -rf -- "${CONFIG_DIR}"
    fi
    return 0
}

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

# A filter that matches no benchmark still exits 0 from Google Benchmark, and heaptrack would then record
# only its startup and registration as if it were the workload (#2987): refuse before capturing anything.
if [[ "${MODE}" = "bench" ]]; then
    # As profile-perf.sh: the names on stdout, one per line; Google Benchmark's complaint on stderr.
    BENCH_LIST_ERR="$(mktemp)"
    BENCH_LIST_STATUS=0
    BENCH_MATCHES="$("${BINARY}" "--benchmark_filter=${BENCH_FILTER}" --benchmark_list_tests=true 2>"${BENCH_LIST_ERR}")" || BENCH_LIST_STATUS=$?
    BENCH_LIST_ERRORS="$(cat "${BENCH_LIST_ERR}")"
    rm -f "${BENCH_LIST_ERR}"
    [[ "${BENCH_LIST_STATUS}" -eq 0 ]] || die "Failed to list benchmarks from ${BINARY} (exit ${BENCH_LIST_STATUS}): ${BENCH_LIST_ERRORS}"
    if [[ "$(printf '%s\n' "${BENCH_MATCHES}" | grep -c . || true)" -eq 0 ]]; then
        die "--bench-filter '${BENCH_FILTER}' matches no benchmarks, so a capture would hold only benchmark startup. Run '${BINARY} --benchmark_list_tests=true' to see valid names."
    fi
fi

# ── config isolation (#2250) ──────────────────────────────────────────────────
if [[ "${MODE}" = "app" ]]; then
    if [[ "${USE_USER_CONFIG}" -eq 1 ]]; then
        # With the user's config, a TaskSmack of theirs already running holds the lock.
        RUNNING_PIDS="$(pgrep -u "$(id -u)" -x "$(basename "${BINARY}")" || true)"
        if [[ -n "${RUNNING_PIDS}" ]]; then
            die "--user-config: TaskSmack is already running (pid ${RUNNING_PIDS//$'\n'/, }). It holds the single-instance lock, so the profiled copy would only show its 'already running' message. Close it first, or drop --user-config to profile with a fresh config directory."
        fi
    else
        CONFIG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tasksmack-profile-config.XXXXXX")"
        trap remove_config_dir EXIT
        # Exported for TaskSmack only: heaptrack and the other tools run here ignore it.
        export TASKSMACK_CONFIG_DIR="${CONFIG_DIR}"
    fi
fi

# ── capture ───────────────────────────────────────────────────────────────────
print_step "Starting heaptrack capture (mode=${MODE}, preset=${PRESET})"
info "Output base: ${HEAPTRACK_BASE}"
info "Binary:      ${BINARY}"
if [[ "${MODE}" = "app" ]]; then
    info "Config dir:  ${CONFIG_DIR:-the user config (--user-config)}"
    info "App log:     ${APP_LOG}"
fi

{
    echo "HEAP_MODE=${MODE}"
    echo "PRESET=${PRESET}"
    echo "BINARY=${BINARY}"
    echo "HEAPTRACK_BASE=${HEAPTRACK_BASE}"
    echo "TIMESTAMP=${TIMESTAMP}"
    if [[ "${MODE}" = "app" ]]; then
        echo "CONFIG_DIR=${CONFIG_DIR:-user config (--user-config)}"
        echo "APP_LOG=${APP_LOG}"
    fi
} > "${LOG_FILE}"

HEAP_EXIT_CODE=0
if [[ "${MODE}" = "app" ]]; then
    echo ""
    echo "Launching TaskSmack under heaptrack. Exercise the application, then close it."
    echo ""
    set +e
    # stdout is also copied to the app log, where the "already running" line is looked for below;
    # heaptrack exits with TaskSmack's own exit code, which PIPESTATUS keeps.
    heaptrack -o "${HEAPTRACK_BASE}" "${BINARY}" 2> >(tee -a "${LOG_FILE}" >&2) | tee "${APP_LOG}"
    HEAP_EXIT_CODE=${PIPESTATUS[0]}
    set -e
    echo "EXIT_CODE=${HEAP_EXIT_CODE}" >> "${LOG_FILE}"
    if [[ "${HEAP_EXIT_CODE}" -eq "${ALREADY_RUNNING_EXIT_CODE}" ]] || grep -q "${ALREADY_RUNNING_MARKER}" "${APP_LOG}" 2>/dev/null; then
        die "TaskSmack exited (code ${HEAP_EXIT_CODE}): another TaskSmack is already running with the same config directory, so this one only showed its 'already running' message and the allocation profile is of nothing worth having. Close the other TaskSmack, or drop --user-config so the capture gets its own config directory. See ${APP_LOG}."
    fi
else
    set +e
    heaptrack -o "${HEAPTRACK_BASE}" "${BINARY}" \
        "--benchmark_filter=${BENCH_FILTER}" \
        "--benchmark_repetitions=${BENCH_REPS}" \
        "--benchmark_min_time=${BENCH_MIN_TIME}" \
        --benchmark_report_aggregates_only=true \
        --benchmark_display_aggregates_only=true 2> >(tee -a "${LOG_FILE}" >&2)
    HEAP_EXIT_CODE=$?
    set -e
fi

# heaptrack writes <base>.<pid>.gz, or <base>.<pid>.zst when zstd is installed -- find either
HEAPTRACK_DATA="$(find "${PERF_DIR}" -maxdepth 1 \( -name "${PREFIX}-${TIMESTAMP}*.gz" -o -name "${PREFIX}-${TIMESTAMP}*.zst" \) -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2- || true)"

if [[ -z "${HEAPTRACK_DATA}" ]]; then
    die "heaptrack did not produce a .gz or .zst output file under ${PERF_DIR}. Check ${LOG_FILE}."
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

exit "${HEAP_EXIT_CODE}"
