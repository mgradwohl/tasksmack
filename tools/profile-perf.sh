#!/usr/bin/env bash
# tools/profile-perf.sh — Capture a Linux perf CPU trace for TaskSmack.
#
# Usage:
#   ./tools/profile-perf.sh app   [--preset <preset>] [--skip-build]
#                                 [--warmup <seconds>] [--duration <seconds>] [--include-startup]
#   ./tools/profile-perf.sh bench [--preset <preset>] [--skip-build]
#                                 [--bench-filter <regex>] [--bench-reps <n>]
#                                 [--bench-min-time <t>]
#
# Modes:
#   app   — launch TaskSmack, let it start up and warm up, then attach perf and record its
#           steady state until it exits (or for --duration seconds). Default preset: profile.
#   bench — run TaskSmackBenchmarks under perf with a benchmark filter. Default preset: benchmark.
#
# App-mode options:
#   --warmup <seconds>   Once TaskSmack's main loop is running, wait this long before perf
#                        attaches, so font/theme loading and the first process enumerations stay
#                        out of the profile. Default: 5. 0 attaches as soon as the loop starts.
#   --duration <seconds> Record for this long, then close TaskSmack (SIGTERM) automatically.
#                        Default: 0 = record until you close TaskSmack (or press Ctrl+C, which
#                        stops the capture and closes TaskSmack).
#   --include-startup    Launch TaskSmack under perf as before, so startup is profiled too, until
#                        you close it. --warmup is ignored; --duration cannot be combined with it.
#
# Steady-state captures wait for TaskSmack to log "Entering main loop" (up to 30 s; launched with
# TASKSMACK_LOG_LEVEL=info unless already set, so release builds log it too), then for --warmup.
# In every app capture, --include-startup included, TaskSmack's own stdout and stderr go to
# perf-app-<timestamp>-app.log; perf's messages stay on the terminal and in the session log.
#
# The run fails if TaskSmack exits before recording starts, before --duration elapses, or with a
# non-zero exit code. When the script closes TaskSmack (after --duration or Ctrl+C), a clean exit
# after SIGTERM (code 0) passes; needing SIGKILL after 10 s, or any other exit code, fails.
#
# The preset, its build directory, build type, compiler and CMAKE_CXX_FLAGS* entries are printed and
# written to the log, so every profile says what it measured. So are the real compile flags of one of
# the profiled binary's src/ files (src/main.cpp for the app), read from the build's
# compile_commands.json, which include add_compile_options() flags such as a TASKSMACK_MARCH -march,
# -stdlib=libc++ and the release hardening flags; the log also has that file's full compile command.
# Without compile_commands.json or python3 the script notes that and logs the cache entries only.
# The default app preset, profile, is an
# -O2 build with frame pointers kept for better stacks; pass --preset release to profile the
# shipped build's code generation instead.
#
# Outputs under perf-data/:
#   perf-<mode>-<timestamp>.data   — perf sample data (pass to analyze-perf.sh or hotspot)
#   perf-<mode>-<timestamp>.log    — session log
#
# Examples:
#   ./tools/profile-perf.sh app
#   ./tools/profile-perf.sh app --preset profile
#   ./tools/profile-perf.sh app --warmup 10 --duration 30
#   ./tools/profile-perf.sh app --include-startup
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
    echo "Usage: $0 app   [--preset <preset>] [--skip-build] [--warmup <seconds>] [--duration <seconds>] [--include-startup]" >&2
    echo "       $0 bench [--preset <preset>] [--skip-build] [--bench-filter <regex>] [--bench-reps <n>] [--bench-min-time <t>]" >&2
    exit 1
fi

PRESET=""
SKIP_BUILD=0
WARMUP_SECONDS=5
DURATION_SECONDS=0
INCLUDE_STARTUP=0
APP_OPTION_GIVEN=""
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
        --warmup)       [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; WARMUP_SECONDS="$2"; APP_OPTION_GIVEN="$1"; shift 2 ;;
        --duration)     [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; DURATION_SECONDS="$2"; APP_OPTION_GIVEN="$1"; shift 2 ;;
        --include-startup) INCLUDE_STARTUP=1; APP_OPTION_GIVEN="$1"; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ "${MODE}" != "app" && -n "${APP_OPTION_GIVEN}" ]]; then
    echo "ERROR: ${APP_OPTION_GIVEN} applies to app mode only" >&2
    exit 1
fi
[[ "${WARMUP_SECONDS}" =~ ^[0-9]+$ ]]   || { echo "ERROR: --warmup must be a whole number of seconds" >&2; exit 1; }
[[ "${DURATION_SECONDS}" =~ ^[0-9]+$ ]] || { echo "ERROR: --duration must be a whole number of seconds" >&2; exit 1; }
if [[ "${INCLUDE_STARTUP}" -eq 1 && "${DURATION_SECONDS}" -gt 0 ]]; then
    echo "ERROR: --duration cannot be combined with --include-startup (close TaskSmack to end that capture)" >&2
    exit 1
fi

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

# What was profiled (#1371): the preset's build type and compiler flags, read from its CMake cache,
# so a profile of the -O2 frame-pointer `profile` build is never mistaken for the shipped release.
BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
cmake_cache_value() {
    local cache="${BUILD_DIR}/CMakeCache.txt"
    [[ -f "${cache}" ]] || return 0
    sed -n "s/^$1:[A-Z]*=//p" "${cache}" | head -n 1
}
BUILD_TYPE="$(cmake_cache_value CMAKE_BUILD_TYPE)"
BUILD_TYPE_UPPER="$(printf '%s' "${BUILD_TYPE}" | tr '[:lower:]' '[:upper:]')"
CXX_FLAGS="$(cmake_cache_value CMAKE_CXX_FLAGS)"
if [[ -n "${BUILD_TYPE_UPPER}" ]]; then
    CXX_FLAGS="${CXX_FLAGS:+${CXX_FLAGS} }$(cmake_cache_value "CMAKE_CXX_FLAGS_${BUILD_TYPE_UPPER}")"
fi
CXX_COMPILER="$(cmake_cache_value CMAKE_CXX_COMPILER)"

# The CMAKE_CXX_FLAGS* cache entries miss everything added with add_compile_options() or
# target_compile_options(): a TASKSMACK_MARCH override's -march, -stdlib=libc++, the release
# hardening flags. So also log the real compile command of one of the profiled binary's own src/
# translation units, from compile_commands.json (every preset sets CMAKE_EXPORT_COMPILE_COMMANDS).
# COMPILE_FLAGS is that command without the compiler, include paths and per-file arguments.
COMPILE_DB="${BUILD_DIR}/compile_commands.json"
COMPILE_TU=""
COMPILE_COMMAND=""
COMPILE_FLAGS=""
COMPILE_NOTE=""
if [[ ! -f "${COMPILE_DB}" ]]; then
    COMPILE_NOTE="no ${COMPILE_DB}; reconfigure the preset to get the full compile command"
elif ! command -v python3 &>/dev/null; then
    COMPILE_NOTE="python3 not found; compile command not read from ${COMPILE_DB}"
else
    # Prints three lines: the translation unit, its full command, and the filtered flags.
    COMPILE_INFO="$(python3 - "${COMPILE_DB}" "${REPO_ROOT}/src/" "$(basename "${BINARY}")" <<'PY' || true
import json, shlex, sys

db_path, src_root, target = sys.argv[1], sys.argv[2], sys.argv[3]
with open(db_path, encoding="utf-8") as f:
    entries = json.load(f)

def is_src(e):
    return e.get("file", "").startswith(src_root)

def from_target(e):
    return f"/{target}.dir/" in e.get("output", "")

def rank(e):
    # The binary's own main.cpp first, then any of its src/ files, then any src/ file.
    if from_target(e) and e["file"].endswith("/src/main.cpp"):
        return 0
    return 1 if from_target(e) else 2

candidates = sorted((e for e in entries if is_src(e)), key=rank)
if not candidates:
    sys.exit(1)
entry = candidates[0]
args = entry.get("arguments") or shlex.split(entry["command"])
flags, i = [], 1
while i < len(args):
    arg = args[i]
    nxt = args[i + 1] if i + 1 < len(args) else ""
    if arg == "-Xclang" and nxt in ("-include-pch", "-include"):
        i += 4  # -Xclang -include-pch -Xclang <path>: the precompiled header
        continue
    if arg in ("-o", "-c", "-MF", "-MT", "-MQ", "-isystem", "-I", "-include"):
        i += 2
        continue
    if not (arg.startswith(("-I", "-isystem", "@")) or arg in ("-MD", "-Winvalid-pch", entry["file"])):
        flags.append(arg)
    i += 1
print(entry["file"])
print(shlex.join(args))
print(" ".join(flags))
PY
)"
    if [[ -n "${COMPILE_INFO}" ]]; then
        COMPILE_TU="$(sed -n 1p <<<"${COMPILE_INFO}")"
        COMPILE_COMMAND="$(sed -n 2p <<<"${COMPILE_INFO}")"
        COMPILE_FLAGS="$(sed -n 3p <<<"${COMPILE_INFO}")"
    else
        COMPILE_NOTE="no src/ translation unit found in ${COMPILE_DB}"
    fi
fi

# In bench mode, warn if the filter matches multiple benchmarks. Google Benchmark scales
# iteration count (not time) to fill --benchmark_min_time per benchmark, so a cheap
# per-call benchmark gets looped far more times than an expensive one to fill the same
# time slot. Since perf sampling is time-based, that gives the cheap benchmark equal (or
# greater) representation in the profile regardless of its real-world importance --
# confirmed directly: profiling the default multi-benchmark filter on a GPU-less machine
# attributed ~40% of total samples to near-zero-cost GPU no-op benchmarks, drowning out
# the genuinely expensive process/system-probe work being measured alongside them.
if [[ "${MODE}" = "bench" ]]; then
    BENCH_LIST_ERR="$(mktemp)"
    trap 'rm -f "${BENCH_LIST_ERR}"' EXIT
    BENCH_LIST_STATUS=0
    BENCH_MATCHES="$("${BINARY}" "--benchmark_filter=${BENCH_FILTER}" --benchmark_list_tests=true 2>"${BENCH_LIST_ERR}")" || BENCH_LIST_STATUS=$?
    if [[ "${BENCH_LIST_STATUS}" -ne 0 ]]; then
        die "Failed to list benchmarks from ${BINARY} (exit ${BENCH_LIST_STATUS}): $(cat "${BENCH_LIST_ERR}")"
    fi
    rm -f "${BENCH_LIST_ERR}"
    trap - EXIT
    BENCH_MATCH_COUNT="$(printf '%s\n' "${BENCH_MATCHES}" | grep -c . || true)"
    if [[ "${BENCH_MATCH_COUNT}" -eq 0 ]]; then
        die "--bench-filter '${BENCH_FILTER}' matches no benchmarks. Capturing would only measure benchmark startup/shutdown noise. Run '${BINARY} --benchmark_list_tests=true' to see valid names."
    elif [[ "${BENCH_MATCH_COUNT}" -gt 1 ]]; then
        echo "WARNING: --bench-filter '${BENCH_FILTER}' matches ${BENCH_MATCH_COUNT} benchmarks:" >&2
        printf '%s\n' "${BENCH_MATCHES}" | sed 's/^/  - /' >&2
        echo "  Profiling several benchmarks together can produce misleading relative" >&2
        echo "  percentages when their per-call costs differ a lot (see the comment above" >&2
        echo "  this check). For accurate hotspot attribution on one function, pass" >&2
        echo "  --bench-filter matching exactly one benchmark." >&2
        echo "" >&2
    fi
fi

# ── capture ───────────────────────────────────────────────────────────────────
print_step "Starting perf capture (mode=${MODE}, preset=${PRESET})"
info "Preset:     ${PRESET}"
info "Build dir:  ${BUILD_DIR}"
info "Build type: ${BUILD_TYPE:-<unknown: no CMakeCache.txt>}"
info "CXX flags:  ${CXX_FLAGS:-<unknown>} (CMAKE_CXX_FLAGS* cache entries only)"
info "Compiler:   ${CXX_COMPILER:-<unknown>}"
if [[ -n "${COMPILE_FLAGS}" ]]; then
    info "Compiled:   ${COMPILE_FLAGS}"
    info "            (from ${COMPILE_TU#"${REPO_ROOT}/"}; full command in the log)"
else
    echo "NOTE: ${COMPILE_NOTE}; only the CMAKE_CXX_FLAGS* cache entries above are known, which miss" >&2
    echo "      add_compile_options()/target_compile_options() flags such as -march or -stdlib." >&2
fi
info "Binary:     ${BINARY}"
info "Data:       ${DATA_FILE}"
info "Log:        ${LOG_FILE}"

{
    echo "PROFILE_MODE=${MODE}"
    echo "PRESET=${PRESET}"
    echo "BUILD_DIR=${BUILD_DIR}"
    echo "BUILD_TYPE=${BUILD_TYPE}"
    echo "CXX_FLAGS=${CXX_FLAGS}"
    echo "CXX_COMPILER=${CXX_COMPILER}"
    if [[ -n "${COMPILE_FLAGS}" ]]; then
        echo "COMPILE_TU=${COMPILE_TU}"
        echo "COMPILE_FLAGS=${COMPILE_FLAGS}"
        echo "COMPILE_COMMAND=${COMPILE_COMMAND}"
    else
        echo "COMPILE_NOTE=${COMPILE_NOTE}"
    fi
    echo "BINARY=${BINARY}"
    echo "DATA_FILE=${DATA_FILE}"
    echo "TIMESTAMP=${TIMESTAMP}"
    echo "PERF_EVENT=${PERF_EVENT}"
    if [[ "${MODE}" = "app" ]]; then
        echo "INCLUDE_STARTUP=${INCLUDE_STARTUP}"
        echo "WARMUP_SECONDS=${WARMUP_SECONDS}"
        echo "DURATION_SECONDS=${DURATION_SECONDS}"
    fi
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
# Every app capture keeps TaskSmack's own stdout/stderr here, apart from perf's messages.
APP_LOG="${PERF_DIR}/${PREFIX}-${TIMESTAMP}-app.log"
if [[ "${MODE}" = "app" && "${INCLUDE_STARTUP}" -eq 1 ]]; then
    info "App log:    ${APP_LOG}"
    echo "APP_LOG=${APP_LOG}" >> "${LOG_FILE}"
    echo ""
    echo "Launching TaskSmack under perf (startup included). Exercise the application, then close it."
    echo ""
    # perf record exits with the workload's exit status, so a crash or early failure fails the run.
    # The workload is a shell that redirects its output to the app log and then execs TaskSmack in
    # the same process, so perf keeps following it and perf's own stderr still reaches the terminal.
    set +e
    # shellcheck disable=SC2016 # $1/$2 are expanded by the inner shell, not here
    perf record "${PERF_RECORD_FLAGS[@]}" -- /bin/sh -c 'exec "$1" > "$2" 2>&1' sh "${BINARY}" "${APP_LOG}" \
        2> >(tee -a "${LOG_FILE}" >&2)
    PERF_EXIT_CODE=$?
    set -e
    echo "EXIT_CODE=${PERF_EXIT_CODE}" >> "${LOG_FILE}"
elif [[ "${MODE}" = "app" ]]; then
    # Steady-state capture (#1371): launch TaskSmack on its own, wait for its main loop plus a
    # warm-up, then attach perf to the running process, so startup costs (fonts, themes, the first
    # process enumeration) stay out of the profile.
    MAIN_LOOP_TIMEOUT_SECONDS=30
    MAIN_LOOP_MARKER="Entering main loop" # Logged by Core::Application::run() at info level
    APP_PID=""
    APP_EXIT_CODE=""
    INTERRUPTED=0

    app_alive() { [[ -n "${APP_PID}" ]] && kill -0 "${APP_PID}" 2>/dev/null; }
    reap_app() {
        [[ -n "${APP_PID}" ]] || return 0
        set +e
        wait "${APP_PID}"
        APP_EXIT_CODE=$?
        set -e
        APP_PID=""
    }
    # SIGTERM (SDL turns it into a normal quit), then SIGKILL if it hasn't exited after 10 s.
    APP_KILLED=0
    stop_app() {
        [[ -n "${APP_PID}" ]] || return 0
        if app_alive; then
            kill -TERM "${APP_PID}" 2>/dev/null || true
            local i
            for ((i = 0; i < 20; i++)); do
                app_alive || break
                sleep 0.5
            done
            if app_alive; then
                kill -KILL "${APP_PID}" 2>/dev/null || true
                APP_KILLED=1
            fi
        fi
        reap_app
    }
    # Fails the run if TaskSmack is gone or the user pressed Ctrl+C before recording started.
    check_still_waiting() {
        if [[ "${INTERRUPTED}" -eq 1 ]]; then
            stop_app
            die "Interrupted before recording started; nothing was captured."
        fi
        if ! app_alive; then
            reap_app
            echo "APP_EXIT_CODE=${APP_EXIT_CODE}" >> "${LOG_FILE}"
            die "TaskSmack exited (code ${APP_EXIT_CODE}) before recording started; nothing was captured. See ${APP_LOG}."
        fi
    }

    # A background job in a non-interactive shell ignores SIGINT, so Ctrl+C reaches perf and this
    # script but not TaskSmack: note it here and close TaskSmack ourselves. Never leave it running.
    trap 'INTERRUPTED=1' INT
    trap 'stop_app' EXIT

    info "App log:    ${APP_LOG}"
    echo "APP_LOG=${APP_LOG}" >> "${LOG_FILE}"
    # Release builds log at warn by default; info makes them print the main-loop marker too.
    # A TASKSMACK_LOG_LEVEL already in the environment wins.
    TASKSMACK_LOG_LEVEL="${TASKSMACK_LOG_LEVEL:-info}" "${BINARY}" > "${APP_LOG}" 2>&1 &
    APP_PID=$!
    echo "APP_PID=${APP_PID}" >> "${LOG_FILE}"

    echo ""
    echo "Launched TaskSmack (pid ${APP_PID}). Waiting for its main loop (up to ${MAIN_LOOP_TIMEOUT_SECONDS}s)..."
    MAIN_LOOP_DEADLINE=$((SECONDS + MAIN_LOOP_TIMEOUT_SECONDS))
    MAIN_LOOP_SEEN=0
    while ((SECONDS < MAIN_LOOP_DEADLINE)); do
        check_still_waiting
        if grep -q "${MAIN_LOOP_MARKER}" "${APP_LOG}" 2>/dev/null; then
            MAIN_LOOP_SEEN=1
            break
        fi
        sleep 0.25
    done
    check_still_waiting
    if [[ "${MAIN_LOOP_SEEN}" -eq 0 ]]; then
        echo "WARNING: TaskSmack did not log '${MAIN_LOOP_MARKER}' within ${MAIN_LOOP_TIMEOUT_SECONDS}s; continuing with the warm-up alone." >&2
    fi
    echo "MAIN_LOOP_SEEN=${MAIN_LOOP_SEEN}" >> "${LOG_FILE}"

    if [[ "${WARMUP_SECONDS}" -gt 0 ]]; then
        echo "Warming up for ${WARMUP_SECONDS}s before perf attaches..."
        for ((i = 0; i < WARMUP_SECONDS; i++)); do
            check_still_waiting
            sleep 1
        done
        check_still_waiting
    fi

    PERF_TARGET_ARGS=(-p "${APP_PID}")
    echo ""
    if [[ "${DURATION_SECONDS}" -gt 0 ]]; then
        PERF_TARGET_ARGS+=(-- sleep "${DURATION_SECONDS}")
        echo "Recording TaskSmack for ${DURATION_SECONDS}s; it is closed automatically afterwards."
    else
        echo "Recording TaskSmack. Exercise the application, then close it (Ctrl+C also stops the capture and closes it)."
    fi
    echo ""
    set +e
    perf record "${PERF_RECORD_FLAGS[@]}" "${PERF_TARGET_ARGS[@]}" 2> >(tee -a "${LOG_FILE}" >&2)
    PERF_EXIT_CODE=$?
    set -e
    echo "EXIT_CODE=${PERF_EXIT_CODE}" >> "${LOG_FILE}"

    # perf record -p exits 0 when its target exits, whatever the target's exit code, so check the
    # app's own status: a crash, or an exit before --duration elapsed, fails the run.
    if app_alive; then
        # TaskSmack quits cleanly on SIGTERM: SDL turns it into SDL_EVENT_QUIT, the main loop ends and
        # main() returns 0. Anything else -- the SIGKILL fallback for a hang (137), or a crash during
        # shutdown (e.g. 134 for SIGABRT, 139 for SIGSEGV) -- fails the run.
        stop_app
        if [[ "${APP_KILLED}" -eq 1 ]]; then
            END_REASON="did not exit within 10s of SIGTERM and was killed with SIGKILL"
            APP_FAILED=1
        elif [[ "${APP_EXIT_CODE}" -ne 0 ]]; then
            END_REASON="exited with a non-zero code after the script's SIGTERM (after --duration or Ctrl+C)"
            APP_FAILED=1
        else
            END_REASON="closed cleanly by the script's SIGTERM (after --duration or Ctrl+C)"
            APP_FAILED=0
        fi
    else
        reap_app
        if [[ "${DURATION_SECONDS}" -gt 0 && "${INTERRUPTED}" -eq 0 ]]; then
            END_REASON="exited on its own before --duration elapsed"
            APP_FAILED=1
        elif [[ "${APP_EXIT_CODE}" -ne 0 ]]; then
            END_REASON="exited with a non-zero code"
            APP_FAILED=1
        else
            END_REASON="closed by the user"
            APP_FAILED=0
        fi
    fi
    {
        echo "APP_EXIT_CODE=${APP_EXIT_CODE}"
        echo "APP_END_REASON=${END_REASON}"
    } >> "${LOG_FILE}"
    trap - INT EXIT
    if [[ "${APP_FAILED}" -eq 1 ]]; then
        die "TaskSmack ${END_REASON} (code ${APP_EXIT_CODE}). The trace at ${DATA_FILE} was kept but the run failed; see ${APP_LOG}."
    fi
    info "TaskSmack ${END_REASON} (code ${APP_EXIT_CODE})."
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
