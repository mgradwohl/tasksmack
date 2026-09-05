#!/usr/bin/env bash
# tools/profile-perf.sh — Capture a Linux perf CPU trace for TaskSmack.
#
# Usage:
#   ./tools/profile-perf.sh app   [--preset <preset>] [--skip-build]
#   ./tools/profile-perf.sh bench [--preset <preset>] [--skip-build]
#                                 [--bench-filter <regex>] [--bench-reps <n>]
#                                 [--bench-min-time <t>]
#
# Modes:
#   app   — launch TaskSmack, wait for it to exit, then stop perf. Default preset: profile.
#   bench — run TaskSmackBenchmarks under perf with a benchmark filter. Default preset: benchmark.
#
# Outputs under perf-data/:
#   perf-<mode>-<timestamp>.data   — perf sample data (pass to analyze-perf.sh or hotspot)
#   perf-<mode>-<timestamp>.log    — session log
#
# Examples:
#   ./tools/profile-perf.sh app
#   ./tools/profile-perf.sh app --preset profile
#   ./tools/profile-perf.sh bench --bench-filter 'BM_ProcessModel_Refresh$'
#   ./tools/profile-perf.sh bench --preset profile --bench-filter 'BM_(ProcessProbe|ProcessModel)'
#
# Requirements:
#   perf  — ships with linux-tools-$(uname -r); install linux-tools-generic as a fallback.
#   On WSL2: kernel version mismatch between linux-tools and the WSL kernel is common.
#            See https://github.com/microsoft/WSL/issues for workarounds; or profile on
#            a native Linux machine / GitHub Actions runner.
#   Also on WSL2 (a separate issue from the above): the hypervisor does not pass through
#   real hardware performance counters, so the default `cycles` event silently records
#   zero samples instead of erroring — this script detects that and falls back to the
#   `cpu-clock` software event automatically, and fails loudly if a capture still ends up
#   empty. The same fallback applies to most containers and some cloud VMs/CI runners.

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
BENCH_REPS=5
BENCH_MIN_TIME="0.5s"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)       [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; PRESET="$2";        shift 2 ;;
        --skip-build)   SKIP_BUILD=1;        shift   ;;
        --bench-filter) [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_FILTER="$2";  shift 2 ;;
        --bench-reps)   [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_REPS="$2";    shift 2 ;;
        --bench-min-time) [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_MIN_TIME="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Default preset depends on mode
if [[ -z "${PRESET}" ]]; then
    PRESET=$([ "${MODE}" = "app" ] && echo "profile" || echo "benchmark")
fi

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
PREFIX="perf-${MODE}"
DATA_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}.data"
LOG_FILE="${PERF_DIR}/${PREFIX}-${TIMESTAMP}.log"

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

print_step() {
    echo
    echo "──────────────────────────────────────────"
    echo "  $*"
    echo "──────────────────────────────────────────"
}

# Detect WSL2 kernel/tools mismatch and warn; does not abort.
check_perf_version() {
    local kernel_ver
    kernel_ver="$(uname -r)"

    # perf --version output: "perf version X.Y.Z" or error if mismatched
    local perf_out
    perf_out="$(perf --version 2>&1 || true)"

    if echo "${perf_out}" | grep -qi "not found for kernel"; then
        echo "WARNING: perf kernel/tools version mismatch detected." >&2
        echo "  Running kernel:  ${kernel_ver}" >&2
        echo "  Suggested fix:   sudo apt install linux-tools-$(uname -r) linux-tools-generic" >&2
        echo "  On WSL2: install the WSL2-specific tools package or profile on native Linux." >&2
        echo "" >&2
    fi
}

# Detect a perf event that actually produces samples on this host. Hardware `cycles` is
# the most accurate when the kernel has real PMU access, but under WSL2 (no hardware
# counter passthrough), many containers, and some cloud VMs/CI runners, `perf record`
# with `cycles` does not error -- it silently writes a data file with zero samples,
# which only surfaces later as a confusing "has no samples" error from `perf report`.
# Probe for it up front instead and fall back to the `cpu-clock` software event, which
# works everywhere `perf record` itself works.
detect_perf_event() {
    if perf stat -e cycles -- true &>/dev/null; then
        echo "cycles"
    elif perf stat -e cpu-clock -- true &>/dev/null; then
        echo "cpu-clock"
    else
        die "Neither the 'cycles' nor 'cpu-clock' perf event works in this environment. This usually means /proc/sys/kernel/perf_event_paranoid is too restrictive, or perf lacks the necessary permissions/capabilities. Check 'cat /proc/sys/kernel/perf_event_paranoid' (0-1 is typically needed) or run with appropriate privileges."
    fi
}

# ── pre-flight ────────────────────────────────────────────────────────────────
print_step "Checking prerequisites"

check_command perf "apt install linux-tools-generic linux-tools-$(uname -r 2>/dev/null || echo 'generic')" || \
    die "perf is required. On Ubuntu: sudo apt install linux-tools-generic"

check_perf_version

PERF_EVENT="$(detect_perf_event)"
if [[ "${PERF_EVENT}" != "cycles" ]]; then
    echo "NOTE: hardware 'cycles' event unavailable (common on WSL2/containers/some cloud VMs)." >&2
    echo "      Falling back to the '${PERF_EVENT}' software event." >&2
    echo "" >&2
fi

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
print_step "Starting perf capture (mode=${MODE}, preset=${PRESET})"
info "Data:   ${DATA_FILE}"
info "Log:    ${LOG_FILE}"
info "Binary: ${BINARY}"

{
    echo "PROFILE_MODE=${MODE}"
    echo "PRESET=${PRESET}"
    echo "BINARY=${BINARY}"
    echo "DATA_FILE=${DATA_FILE}"
    echo "TIMESTAMP=${TIMESTAMP}"
    echo "PERF_EVENT=${PERF_EVENT}"
} > "${LOG_FILE}"

# perf record flags:
#   -e            — sampling event; see detect_perf_event() above
#   -F 997        — sample at ~997 Hz (prime to avoid aliasing with timer interrupts)
#   -g            — capture call graphs
#   --call-graph dwarf — use DWARF unwinding (more accurate than fp for inlined frames;
#                         requires -g in build flags; falls back gracefully on older kernels)
#   -o            — output file
PERF_RECORD_FLAGS=(-e "${PERF_EVENT}" -F 997 -g --call-graph dwarf -o "${DATA_FILE}")

PERF_EXIT_CODE=0
if [[ "${MODE}" = "app" ]]; then
    echo ""
    echo "Launching TaskSmack under perf. Exercise the application, then close it."
    echo ""
    set +e
    perf record "${PERF_RECORD_FLAGS[@]}" -- "${BINARY}" 2> >(tee -a "${LOG_FILE}" >&2)
    PERF_EXIT_CODE=$?
    set -e
    echo "EXIT_CODE=${PERF_EXIT_CODE}" >> "${LOG_FILE}"
else
    set +e
    perf record "${PERF_RECORD_FLAGS[@]}" -- \
        "${BINARY}" \
        "--benchmark_filter=${BENCH_FILTER}" \
        "--benchmark_repetitions=${BENCH_REPS}" \
        "--benchmark_min_time=${BENCH_MIN_TIME}" \
        --benchmark_report_aggregates_only=true \
        --benchmark_display_aggregates_only=true 2> >(tee -a "${LOG_FILE}" >&2)
    PERF_EXIT_CODE=$?
    set -e
    echo "EXIT_CODE=${PERF_EXIT_CODE}" >> "${LOG_FILE}"
fi

# ── validate ──────────────────────────────────────────────────────────────────
# A misconfigured event (or one silently unsupported on this host) can make perf record
# exit 0 while writing a data file with zero samples -- confusing to discover only later,
# from analyze-perf.sh or `perf report` failing with "has no samples". Check now instead.
if [[ "${PERF_EXIT_CODE}" -eq 0 ]]; then
    PERF_SCRIPT_OUT="$(mktemp)"
    PERF_SCRIPT_ERR="$(mktemp)"
    # Clean up unconditionally on exit from this point (success, the die() below, or any
    # unexpected error) rather than only on the success path -- die() calls exit
    # immediately, so an `rm -f` placed after the check it guards would never run.
    trap 'rm -f "${PERF_SCRIPT_OUT}" "${PERF_SCRIPT_ERR}"' EXIT
    # Separate stdout/stderr into files rather than piping into `wc -l` with stderr
    # discarded: under `set -e`+`pipefail`, a genuine `perf script` failure (not just an
    # empty trace) would otherwise abort the script right here with no diagnostic at all,
    # since the failure surfaces as this command's own exit status, not as sample count 0.
    # -G (hide call graph) makes this an accurate one-line-per-sample count: without it,
    # DWARF call-graph output emits one line per stack frame, so `wc -l` on the default
    # output counts frames, not samples (confirmed: 38855 frame-lines vs. 11810 actual
    # samples on the same trace, per perf record's own reported count).
    if ! perf script -G -i "${DATA_FILE}" > "${PERF_SCRIPT_OUT}" 2> "${PERF_SCRIPT_ERR}"; then
        die "perf script failed while validating the capture at ${DATA_FILE}: $(cat "${PERF_SCRIPT_ERR}")"
    fi
    SAMPLE_COUNT="$(wc -l < "${PERF_SCRIPT_OUT}")"
    if [[ "${SAMPLE_COUNT}" -eq 0 ]]; then
        die "perf captured zero samples (event=${PERF_EVENT}). The trace at ${DATA_FILE} is empty and unusable. This usually means the workload exited before perf attached, or ran too briefly at 997 Hz to catch anything -- try a longer-running workload or a lower -F rate."
    fi
    info "Captured ${SAMPLE_COUNT} samples."
fi

# ── done ──────────────────────────────────────────────────────────────────────
print_step "Capture complete"
echo ""
echo "PERF_DATA=${DATA_FILE}"
echo "LOG=${LOG_FILE}"
echo ""
echo "Next steps:"
if command -v hotspot &>/dev/null; then
    echo "  hotspot ${DATA_FILE}                     # GUI flamegraph viewer"
fi
echo "  ./tools/analyze-perf.sh ${DATA_FILE}        # CLI top functions + flamegraph SVG"
echo "  perf report -i ${DATA_FILE}                 # interactive TUI"

exit "${PERF_EXIT_CODE}"
