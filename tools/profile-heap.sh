#!/usr/bin/env bash
# tools/profile-heap.sh — Heap allocation profiling for TaskSmack using heaptrack.
#
# Usage:
#   ./tools/profile-heap.sh app   [--preset <preset>] [--skip-build] [--user-config] [--duration <seconds>]
#   ./tools/profile-heap.sh bench [--preset <preset>] [--skip-build]
#                                 [--bench-filter <regex>] [--bench-reps <n>]
#                                 [--bench-min-time <t>]
#   ./tools/profile-heap.sh -h | --help   # print this help
#
# Modes:
#   app   — launch TaskSmack under heaptrack, wait for exit (or for --duration). Default preset: profile.
#   bench — run TaskSmackBenchmarks under heaptrack. Default preset: benchmark.
#
# App mode runs TaskSmack with a fresh, empty TASKSMACK_CONFIG_DIR, removed afterwards (#2250), so it
# starts from default settings, cannot rewrite yours, and is not stopped by the single-instance lock
# of a TaskSmack you already have open. --user-config runs it with the config it would use on its own
# instead, and refuses to start while a TaskSmack of yours is running. A TaskSmack turned away by
# another instance's lock (it logs "TaskSmack is already running", or exits with code 75) fails the
# run. TaskSmack's stdout is also copied to heaptrack-app-<timestamp>-app.log.
#
# --duration <seconds> (app mode, #3406): once TaskSmack logs "Entering main loop" (up to 30 s; it is launched
# with TASKSMACK_LOG_LEVEL=info unless that is already set, so release builds log it too), record for this long,
# then close TaskSmack with SIGTERM, so an unattended run (an agent, CI, a re-measure) ends on its own, as
# profile-perf.sh's --duration does. The signal goes to TaskSmack itself, not to heaptrack's wrapper script, so
# heaptrack finishes its file normally. The run fails if TaskSmack exits before the main loop or before
# --duration elapses, or exits non-zero after the signal. 0 (the default) waits for you to close it.
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
#   ./tools/profile-heap.sh app --duration 60     # unattended: 60 s of steady state, then closed
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
    echo "Usage: $0 app   [--preset <preset>] [--skip-build] [--user-config] [--duration <seconds>]" >&2
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
DURATION_SECONDS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)         [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; PRESET="$2";           shift 2 ;;
        --skip-build)     SKIP_BUILD=1;           shift   ;;
        --bench-filter)   [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_FILTER="$2";     shift 2 ;;
        --bench-reps)     [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_REPS="$2";       shift 2 ;;
        --bench-min-time) [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; BENCH_MIN_TIME="$2";   shift 2 ;;
        --user-config)    USE_USER_CONFIG=1;      shift   ;;
        --duration)       [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; DURATION_SECONDS="$2"; shift 2 ;;
        -h|--help)        sed -n '2,/^$/{/^$/d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ "${MODE}" != "app" && "${USE_USER_CONFIG}" -eq 1 ]]; then
    echo "ERROR: --user-config applies to app mode only" >&2
    exit 1
fi
[[ "${DURATION_SECONDS}" =~ ^[0-9]+$ ]] || { echo "ERROR: --duration must be a whole number of seconds" >&2; exit 1; }
DURATION_SECONDS=$((10#${DURATION_SECONDS}))
if [[ "${MODE}" != "app" && "${DURATION_SECONDS}" -gt 0 ]]; then
    echo "ERROR: --duration applies to app mode only" >&2
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
        echo "DURATION_SECONDS=${DURATION_SECONDS}"
    fi
} > "${LOG_FILE}"

HEAP_EXIT_CODE=0
if [[ "${MODE}" = "app" && "${DURATION_SECONDS}" -gt 0 ]]; then
    # Unattended (#3406): heaptrack in the background, TaskSmack found as its child, closed after --duration.
    MAIN_LOOP_MARKER="Entering main loop" # Logged by Core::Application::run() at info level
    export TASKSMACK_LOG_LEVEL="${TASKSMACK_LOG_LEVEL:-info}"
    HEAP_PID=""
    APP_PID=""
    APP_KILLED=0
    INTERRUPTED=0
    alive() { [[ -n "$1" ]] && kill -0 "$1" 2>/dev/null; }
    # Closes the TaskSmack this run started -- SIGTERM (SDL turns it into a normal quit), SIGKILL if it is
    # still there 10 s later, as profile-perf.sh's stop_app() -- then reaps heaptrack, giving it 30 s to
    # finish its file before it is killed too. Safe to call twice; the EXIT trap calls it on every exit.
    # heaptrack runs in its own session (setsid below), so its process group holds heaptrack, its
    # interpreter and TaskSmack: when TaskSmack wasn't found yet (a Ctrl+C during startup), or heaptrack won't
    # finish, the whole group is signalled, so a TaskSmack launched after the interruption can't outlive
    # the run either.
    # shellcheck disable=SC2317,SC2329 # also invoked from the EXIT trap
    stop_capture() {
        local i
        if [[ -z "${APP_PID}" && -n "${HEAP_PID}" ]]; then
            APP_PID="$(pgrep -P "${HEAP_PID}" -f -- "${BINARY_PATTERN}" | head -1 || true)"
        fi
        if [[ -z "${APP_PID}" ]] && alive "${HEAP_PID}"; then
            kill -TERM -- "-${HEAP_PID}" 2>/dev/null || true
        fi
        if alive "${APP_PID}"; then
            kill -TERM "${APP_PID}" 2>/dev/null || true
            for ((i = 0; i < 20; i++)); do alive "${APP_PID}" || break; sleep 0.5; done
            if alive "${APP_PID}"; then
                kill -KILL "${APP_PID}" 2>/dev/null || true
                APP_KILLED=1
            fi
        fi
        if [[ -n "${HEAP_PID}" ]]; then
            for ((i = 0; i < 60; i++)); do alive "${HEAP_PID}" || break; sleep 0.5; done
            alive "${HEAP_PID}" && kill -KILL -- "-${HEAP_PID}" 2>/dev/null
            set +e
            wait "${HEAP_PID}" 2>/dev/null
            HEAP_EXIT_CODE=$?
            set -e
            HEAP_PID=""
        fi
        return 0
    }
    # A background job in a non-interactive shell ignores SIGINT, so Ctrl+C reaches this script but not
    # TaskSmack or heaptrack: note it, and never leave either running (profile-perf.sh does the same).
    trap 'INTERRUPTED=1' INT
    trap 'stop_capture; remove_config_dir' EXIT TERM
    echo ""
    echo "Launching TaskSmack under heaptrack; it is closed ${DURATION_SECONDS}s after its main loop starts."
    echo ""
    # heaptrack runs TaskSmack as its own child; matched by its command line (the binary's full path), which
    # holds for the real binary and for a script standing in for it, whose process name is its interpreter's.
    BINARY_PATTERN="$(printf '%s' "${BINARY}" | sed 's/[][\\.*^$+?(){}|]/\\&/g')"
    # Its own session, so HEAP_PID is also its process group's ID (a background job of a non-interactive
    # shell isn't a group leader, so setsid needs no fork and $! is heaptrack's own PID).
    setsid heaptrack -o "${HEAPTRACK_BASE}" "${BINARY}" > >(tee "${APP_LOG}") 2> >(tee -a "${LOG_FILE}" >&2) &
    HEAP_PID=$!
    for ((i = 0; i < 300 && INTERRUPTED == 0; i++)); do
        APP_PID="$(pgrep -P "${HEAP_PID}" -f -- "${BINARY_PATTERN}" | head -1 || true)"
        [[ -n "${APP_PID}" ]] && break
        alive "${HEAP_PID}" || break
        sleep 0.1
    done
    END_REASON=""
    if [[ "${INTERRUPTED}" -eq 1 ]]; then
        END_REASON="was interrupted (Ctrl+C) before the capture finished"
    elif [[ -z "${APP_PID}" ]]; then
        END_REASON="exited, or never logged '${MAIN_LOOP_MARKER}', within 30s of starting"
    else
        for ((i = 0; i < 300 && INTERRUPTED == 0; i++)); do
            grep -q "${MAIN_LOOP_MARKER}" "${APP_LOG}" 2>/dev/null && break
            alive "${APP_PID}" || break
            sleep 0.1
        done
        if [[ "${INTERRUPTED}" -eq 1 ]]; then
            END_REASON="was interrupted (Ctrl+C) before the capture finished"
        elif ! grep -q "${MAIN_LOOP_MARKER}" "${APP_LOG}" 2>/dev/null; then
            END_REASON="exited, or never logged '${MAIN_LOOP_MARKER}', within 30s of starting"
        else
            for ((i = 0; i < DURATION_SECONDS && INTERRUPTED == 0; i++)); do
                alive "${APP_PID}" || break
                sleep 1
            done
            # Checked after the last sleep too: an exit during it is still an exit before --duration elapsed.
            if [[ "${INTERRUPTED}" -eq 1 ]]; then
                END_REASON="was interrupted (Ctrl+C) before --duration elapsed"
            elif ! alive "${APP_PID}"; then
                END_REASON="exited on its own before --duration elapsed"
            fi
        fi
    fi
    stop_capture
    echo "EXIT_CODE=${HEAP_EXIT_CODE}" >> "${LOG_FILE}"
    if [[ -z "${END_REASON}" && "${APP_KILLED}" -eq 1 ]]; then
        END_REASON="did not exit within 10s of the script's SIGTERM and was killed"
    elif [[ -z "${END_REASON}" && "${HEAP_EXIT_CODE}" -ne 0 ]]; then
        END_REASON="exited with code ${HEAP_EXIT_CODE} after the script's SIGTERM"
    fi
    if [[ -n "${END_REASON}" ]]; then
        [[ "${HEAP_EXIT_CODE}" -ne 0 ]] || HEAP_EXIT_CODE=1
        echo "END_REASON=${END_REASON}" >> "${LOG_FILE}"
        if [[ "${HEAP_EXIT_CODE}" -eq "${ALREADY_RUNNING_EXIT_CODE}" ]] || grep -q "${ALREADY_RUNNING_MARKER}" "${APP_LOG}" 2>/dev/null; then
            die "TaskSmack exited (code ${HEAP_EXIT_CODE}): another TaskSmack is already running with the same config directory. See ${APP_LOG}."
        fi
        die "TaskSmack ${END_REASON}, so the allocation profile doesn't cover the --duration window. See ${APP_LOG} and ${LOG_FILE}."
    fi
    echo "END_REASON=closed cleanly by the script's SIGTERM after --duration" >> "${LOG_FILE}"
    trap remove_config_dir EXIT
    trap - INT TERM
elif [[ "${MODE}" = "app" ]]; then
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
