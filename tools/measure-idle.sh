#!/usr/bin/env bash
# tools/measure-idle.sh — Measure TaskSmack's idle CPU% (per thread) and frame time on Linux.
#
# Usage:
#   ./tools/measure-idle.sh [--preset <preset>] [--skip-build] [--warmup <seconds>]
#                           [--duration <seconds>] [--label <name>] [--setup-cmd <command>]
#                           [--synthetic <spec>] [--repeat <N>] [--json <path>]
#                           [--fail-above <pct>]
#
# Each repetition launches TaskSmack with TASKSMACK_TRACE_RESIZE_PERF=1, waits for its main loop
# and a warm-up (as tools/profile-perf.sh app mode does), samples per-thread CPU for --duration
# seconds, closes it with SIGTERM, then prints one table:
#   - per-thread average CPU% (100% = one logical CPU fully busy), and the process total;
#   - app CPU: the process total minus Mesa's software-render/driver worker threads (see below);
#   - fps: presented frames per second, from the deliver-to-deliver loop intervals in the
#     ResizePerf summaries;
#   - frame p95/p99/max: work per presented frame (update+render+post+swap);
#   - loop p95/p99: deliver-to-deliver interval (frame end to frame end, skipped renders included).
# After the last repetition it prints the mean, median and p95 of total CPU, app CPU and fps across
# the repetitions, writes everything as JSON, and applies --fail-above.
#
# App CPU vs total CPU: under WSLg (and any Linux without a GPU driver) Mesa renders through its
# llvmpipe software rasterizer, whose worker threads run inside TaskSmack's process and usually
# dominate its CPU. They measure the CPU rasterizer, not TaskSmack, so whole-process CPU says little
# there. App CPU leaves out threads whose name matches MESA_THREAD_REGEX below:
#   llvmpipe-N   llvmpipe's rasterizer threads (one per logical CPU)
#   lp:*         llvmpipe's compute-shader / setup thread pool threads
#   gdrv*        Gallium threaded-context driver queue (u_threaded_context)
#   glthread*    Mesa's GL marshalling thread (mesa_glthread)
#   disk$*, shdr*, cso*  Mesa's shader-cache, shader-compile and CSO util_queue workers
# Both figures are reported; app CPU is the one the idle target and --fail-above apply to. With the
# /proc sampler a Mesa thread that starts during the sample is not in the per-thread rows, so its
# CPU stays in app CPU (it errs high, never low).
#
# The frame figures come from TaskSmack's periodic ResizePerf summaries (every 5 s at idle), which
# the app logs on its own schedule, so they cannot cover exactly the CPU sample. The script uses
# the summaries logged while CPU was being sampled and prints the span they actually cover (from
# the summary before the first one to the last one) beside the CPU sample's own start and end, so
# the two time ranges are visible instead of assumed equal. They differ by up to one summary
# interval at each end. Percentiles are the worst of those summaries (each summary's p95/p99 is
# over a rolling window of up to 200 samples, which also reaches back before the span when the
# warm-up is short); max is the largest per-interval max among them.
#
# Options:
#   --preset <preset>     CMake preset whose bin/TaskSmack is measured. Default: profile.
#   --skip-build          Measure the existing build instead of configuring and building first.
#   --warmup <seconds>    Wait this long after the main loop starts (and after --setup-cmd) before
#                         sampling, so startup work stays out of the numbers. Default: 15, long
#                         enough at idle frame rates (20-60 fps) for the 200-sample percentile
#                         window to hold no startup or tab-switch frames when sampling begins; 45
#                         when --label contains "minimized" (or "minimised"), since a minimized
#                         window presents only ~5 fps and needs that long to refill the window.
#   --duration <seconds>  Sample for this long. Default: 30.
#   --label <name>        Scenario name printed with the results (e.g. overview, processes).
#                         Default: idle.
#   --setup-cmd <command> Run this shell command once the main loop is up, before the warm-up:
#                         e.g. an xdotool/XTest click that switches to the tab being measured. It
#                         gets TASKSMACK_PID in its environment. Fails the run if it fails. It runs
#                         once per repetition.
#   --synthetic <spec>    Measure the synthetic large-UI scenario instead of this machine: launch
#                         TaskSmack with TASKSMACK_SYNTHETIC=<spec>, e.g. processes=5000,history=full
#                         (keys: processes, cores, disks, interfaces, seed, history, refresh; see
#                         src/App/SyntheticScenario.h). A TASKSMACK_SYNTHETIC already in the
#                         environment is passed through too; this flag overrides it, and
#                         --synthetic '' turns an inherited one off. The spec is
#                         printed with the results and in the RESULT line as synthetic=<spec>.
#   --repeat <N>          Run the whole launch/warm-up/sample/close cycle N times (a fresh process
#                         each time) and aggregate. Default: 1. Use 5 for a baseline or a gate.
#   --json <path>         Write the results as JSON here. Default:
#                         perf-data/idle-<label>-<timestamp>.json, beside the app logs.
#   --fail-above <pct>    Exit with status 3 when the median app CPU across the repetitions is
#                         above <pct> (% of one logical CPU). The JSON is written first.
#   -h, --help            Show this help.
#
# Aggregates: mean; median (the mean of the two middle values for an even count); p95 by nearest
# rank, so with fewer than 20 repetitions it is the largest value.
#
# The JSON (schema "tasksmack-idle/1") holds every repetition's figures and per-thread rows, the
# aggregates, and provenance: git commit and dirty flag (tracked files only), preset and
# CMAKE_BUILD_TYPE, the refresh interval (read from the app log, which logs the background
# samplers' interval) and history window (the synthetic scenario's when it sets one, else the
# config file the app logged it loaded, else HISTORY_SECONDS_DEFAULT from
# src/Domain/SamplingConfig.h), the synthetic spec, the GL renderer, the display refresh rate (from
# xrandr; null when unavailable), CPU model, logical CPU count, MHz, kernel release and whether it
# ran under WSL. No host or user names are recorded.
#
# CPU sampling uses `pidstat -u -t -p <pid> 1 <duration>` (sysstat) when installed, otherwise the
# utime+stime deltas of /proc/<pid>/task/*/stat over the same window. Threads that start or exit
# during the window are left out of the per-thread rows; the total always counts them.
#
# TaskSmack's own output goes to perf-data/idle-<label>-<timestamp>-app.log (-r<N>-app.log per
# repetition when --repeat is above 1). The run fails if TaskSmack exits early, exits non-zero after
# SIGTERM, or needs SIGKILL. Exit status: 0 success, 1 error, 3 --fail-above exceeded.
#
# On WSL (WSLg), the app CPU figures are valid evidence; total CPU is dominated by llvmpipe and
# frame-time figures are not representative of a native compositor/GPU driver. See CONTRIBUTING.md,
# "Measuring idle CPU and frame time", for the idle-CPU target and the baseline it came from.
#
# Examples:
#   ./tools/measure-idle.sh
#   ./tools/measure-idle.sh --skip-build --repeat 5 --fail-above 25
#   ./tools/measure-idle.sh --preset debug --skip-build --duration 30 --label overview
#   ./tools/measure-idle.sh --skip-build --label processes --setup-cmd 'sleep 2; xdotool mousemove <x> <y> click 1'
#   ./tools/measure-idle.sh --skip-build --label synthetic-overview --synthetic processes=5000,history=full --repeat 5

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

PERF_DIR="${REPO_ROOT}/perf-data"
SAMPLING_CONFIG_H="${REPO_ROOT}/src/Domain/SamplingConfig.h"

# Mesa software-render / driver worker threads, left out of app CPU (see the header for each).
MESA_THREAD_REGEX='^(llvmpipe-[0-9]+|lp:.*|gdrv.*|glthread.*|disk[$].*|shdr.*|cso.*)$'

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

PRESET="profile"
SKIP_BUILD=0
WARMUP_SECONDS="" # default chosen from --label below
DURATION_SECONDS=30
LABEL="idle"
SETUP_CMD=""
SYNTHETIC="${TASKSMACK_SYNTHETIC:-}"
REPEAT=1
JSON_PATH=""
FAIL_ABOVE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)     [[ $# -ge 2 ]] || die "$1 requires a value"; PRESET="$2";           shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --warmup)     [[ $# -ge 2 ]] || die "$1 requires a value"; WARMUP_SECONDS="$2";   shift 2 ;;
        --duration)   [[ $# -ge 2 ]] || die "$1 requires a value"; DURATION_SECONDS="$2"; shift 2 ;;
        --label)      [[ $# -ge 2 ]] || die "$1 requires a value"; LABEL="$2";            shift 2 ;;
        --setup-cmd)  [[ $# -ge 2 ]] || die "$1 requires a value"; SETUP_CMD="$2";        shift 2 ;;
        --synthetic)  [[ $# -ge 2 ]] || die "$1 requires a value"; SYNTHETIC="$2";        shift 2 ;;
        --repeat)     [[ $# -ge 2 ]] || die "$1 requires a value"; REPEAT="$2";           shift 2 ;;
        --json)       [[ $# -ge 2 ]] || die "$1 requires a value"; JSON_PATH="$2";        shift 2 ;;
        --fail-above) [[ $# -ge 2 ]] || die "$1 requires a value"; FAIL_ABOVE="$2";       shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${WARMUP_SECONDS}" ]]; then
    # ~5 fps minimized (MINIMIZED_FRAME_SLEEP_MS = 200) vs 20+ fps idle: 200 samples take ~40 s.
    if [[ "${LABEL,,}" =~ minimi[sz]ed ]]; then
        WARMUP_SECONDS=45
    else
        WARMUP_SECONDS=15
    fi
fi
[[ "${WARMUP_SECONDS}" =~ ^[0-9]+$ ]]   || die "--warmup must be a whole number of seconds"
[[ "${DURATION_SECONDS}" =~ ^[0-9]+$ ]] || die "--duration must be a whole number of seconds"
[[ "${REPEAT}" =~ ^[0-9]+$ ]]           || die "--repeat must be a whole number"
# Base 10: Bash reads a leading zero as octal in -gt and $(( )), so "08" would abort.
WARMUP_SECONDS=$((10#${WARMUP_SECONDS}))
DURATION_SECONDS=$((10#${DURATION_SECONDS}))
REPEAT=$((10#${REPEAT}))
[[ "${DURATION_SECONDS}" -gt 0 ]] || die "--duration must be at least 1 second"
[[ "${REPEAT}" -gt 0 ]] || die "--repeat must be at least 1"
[[ -z "${FAIL_ABOVE}" || "${FAIL_ABOVE}" =~ ^[0-9]+(\.[0-9]+)?$ ]] \
    || die "--fail-above must be a non-negative number (percent of one logical CPU)"
[[ "${LABEL}" =~ ^[A-Za-z0-9._-]+$ ]] || die "--label may only contain letters, digits, '.', '_' and '-'"
[[ -r /proc/self/stat ]] || die "/proc is required (Linux only)"
# Letters, digits and = , . _ - only: it goes into the RESULT line as one key=value field.
[[ -z "${SYNTHETIC}" || "${SYNTHETIC}" =~ ^[A-Za-z0-9=,._-]+$ ]] \
    || die "--synthetic may only contain letters, digits, '=', ',', '.', '_' and '-' (e.g. processes=5000,history=full)"

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    validate_build_prereqs || die "Build prerequisites not met."
    echo "Building preset=${PRESET}..."
    cmake --preset "${PRESET}"
    cmake --build --preset "${PRESET}"
fi

BINARY="${REPO_ROOT}/build/${PRESET}/bin/TaskSmack"
[[ -x "${BINARY}" ]] || die "Binary not found or not executable: ${BINARY}. Build with: cmake --build --preset ${PRESET}"

mkdir -p "${PERF_DIR}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
[[ -n "${JSON_PATH}" ]] || JSON_PATH="${PERF_DIR}/idle-${LABEL}-${TIMESTAMP}.json"
mkdir -p "$(dirname "${JSON_PATH}")"
WORK_DIR="$(mktemp -d)"
# One line per repetition: rep total app mesa fps frameP95 frameP99 frameMax loopP95 loopP99
# summaries cpuStart cpuEnd traceStart traceEnd traceSpan appLog
REPS_FILE="${WORK_DIR}/reps"
: > "${REPS_FILE}"

MAIN_LOOP_TIMEOUT_SECONDS=30
MAIN_LOOP_MARKER="Entering main loop" # Logged by Core::Application::run() at info level
APP_PID=""
APP_EXIT_CODE=""
APP_KILLED=0
CLK_TCK="$(getconf CLK_TCK)"
IS_WSL=0
grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null && IS_WSL=1

app_alive() { [[ -n "${APP_PID}" ]] && kill -0 "${APP_PID}" 2>/dev/null; }
reap_app() {
    [[ -n "${APP_PID}" ]] || return 0
    set +e
    wait "${APP_PID}"
    APP_EXIT_CODE=$?
    set -e
    APP_PID=""
}
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
cleanup() {
    stop_app
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check_alive() {
    if ! app_alive; then
        reap_app
        die "TaskSmack exited (code ${APP_EXIT_CODE}) $1. See ${APP_LOG}."
    fi
}

# Prints "utime+stime ticks" of a /proc stat file; the comm field may hold spaces or ')' so the
# fields are counted from the last ')'.
stat_ticks() {
    local line rest
    line="$(cat "$1" 2>/dev/null)" || return 1
    rest="${line##*) }"
    # rest starts at field 3 (state); utime and stime are fields 14 and 15 → rest fields 12 and 13.
    awk '{ print $12 + $13 }' <<<"${rest}"
}

# Prints "tid ticks name" for every thread of the app.
snapshot_threads() {
    local task tid ticks name
    for task in /proc/"${APP_PID}"/task/*; do
        tid="${task##*/}"
        ticks="$(stat_ticks "${task}/stat")" || continue
        name="$(cat "${task}/comm" 2>/dev/null)" || continue
        printf '%s %s %s\n' "${tid}" "${ticks}" "${name// /_}"
    done
}

# Epoch seconds of a spdlog line ("[2026-10-06 15:13:32.952] ..."); empty if it has none.
log_line_epoch() {
    local stamp
    stamp="$(sed -n 's/^\[\([0-9-]* [0-9:.]*\)\].*/\1/p' <<<"$1")"
    [[ -n "${stamp}" ]] && date -d "${stamp}" +%s.%3N
}
# "HH:MM:SS.mmm → HH:MM:SS.mmm (N.N s)" for two epoch times, or "n/a".
describe_span() {
    if [[ -z "$1" || -z "$2" ]]; then
        echo "n/a"
        return
    fi
    printf '%s → %s (%.1f s)' "$(date -d "@$1" +%H:%M:%S.%3N)" "$(date -d "@$2" +%H:%M:%S.%3N)" \
        "$(awk -v a="$1" -v b="$2" 'BEGIN { print b - a }')"
}

# One launch/warm-up/sample/close cycle. $1 = repetition number. Appends a line to REPS_FILE and
# writes the per-thread rows ("tid name cpu mesa") to ${WORK_DIR}/threads-<rep>.
run_once() {
    local rep="$1"
    local suffix=""
    [[ "${REPEAT}" -gt 1 ]] && suffix="-r${rep}"
    APP_LOG="${PERF_DIR}/idle-${LABEL}-${TIMESTAMP}${suffix}-app.log"
    CPU_RAW="${WORK_DIR}/cpu-${rep}"
    APP_EXIT_CODE=""
    APP_KILLED=0

    echo
    echo "=== Repetition ${rep}/${REPEAT}: preset=${PRESET} label=${LABEL} warmup=${WARMUP_SECONDS}s duration=${DURATION_SECONDS}s synthetic=${SYNTHETIC:-none}"
    info "Binary:  ${BINARY}"
    info "App log: ${APP_LOG}"

    local app_env=(TASKSMACK_TRACE_RESIZE_PERF=1 "TASKSMACK_LOG_LEVEL=${TASKSMACK_LOG_LEVEL:-info}")
    # Always set, even when empty: an explicit --synthetic '' must override an inherited
    # TASKSMACK_SYNTHETIC (an empty value turns the scenario off), so the run matches what is reported.
    app_env+=("TASKSMACK_SYNTHETIC=${SYNTHETIC}")
    # env execs TaskSmack in place, so $! is TaskSmack's own PID.
    env "${app_env[@]}" "${BINARY}" > "${APP_LOG}" 2>&1 &
    APP_PID=$!

    echo "Launched TaskSmack (pid ${APP_PID}). Waiting for its main loop (up to ${MAIN_LOOP_TIMEOUT_SECONDS}s)..."
    local deadline=$((SECONDS + MAIN_LOOP_TIMEOUT_SECONDS))
    local seen=0 i
    while ((SECONDS < deadline)); do
        check_alive "before its main loop started"
        if grep -q "${MAIN_LOOP_MARKER}" "${APP_LOG}" 2>/dev/null; then
            seen=1
            break
        fi
        sleep 0.25
    done
    check_alive "before its main loop started"
    if [[ "${seen}" -eq 0 ]]; then
        echo "WARNING: TaskSmack did not log '${MAIN_LOOP_MARKER}' within ${MAIN_LOOP_TIMEOUT_SECONDS}s; continuing with the warm-up alone." >&2
    fi

    if [[ -n "${SETUP_CMD}" ]]; then
        echo "Running setup command: ${SETUP_CMD}"
        TASKSMACK_PID="${APP_PID}" bash -c "${SETUP_CMD}" || die "--setup-cmd failed: ${SETUP_CMD}"
    fi

    if [[ "${WARMUP_SECONDS}" -gt 0 ]]; then
        echo "Warming up for ${WARMUP_SECONDS}s..."
        for ((i = 0; i < WARMUP_SECONDS; i++)); do
            check_alive "during the warm-up"
            sleep 1
        done
    fi
    check_alive "during the warm-up"

    # ResizePerf summaries logged after this line, while CPU is sampled, supply the frame figures.
    local log_start_line cpu_start
    log_start_line="$(wc -l < "${APP_LOG}")"
    cpu_start="$(date +%s.%3N)"

    if [[ "${SAMPLER}" = "pidstat" ]]; then
        echo "Sampling per-thread CPU with pidstat for ${DURATION_SECONDS}s..."
        LC_ALL=C pidstat -u -t -p "${APP_PID}" 1 "${DURATION_SECONDS}" > "${CPU_RAW}" 2>&1 \
            || { check_alive "while sampling"; die "pidstat failed: $(tail -n 3 "${CPU_RAW}")"; }
        check_alive "while sampling"
    else
        echo "Sampling /proc/${APP_PID}/task/*/stat for ${DURATION_SECONDS}s (install sysstat for pidstat)..."
        local before total_before start_ns after total_after end_ns
        before="$(snapshot_threads)"
        total_before="$(stat_ticks "/proc/${APP_PID}/stat")"
        start_ns="$(date +%s%N)"
        for ((i = 0; i < DURATION_SECONDS; i++)); do
            check_alive "while sampling"
            sleep 1
        done
        check_alive "while sampling"
        after="$(snapshot_threads)"
        total_after="$(stat_ticks "/proc/${APP_PID}/stat")"
        end_ns="$(date +%s%N)"
        # Normalised to the pidstat layout the report below reads: "tid name cpu%" rows plus a total.
        awk -v clk="${CLK_TCK}" -v elapsed_ns="$((end_ns - start_ns))" \
            -v total_before="${total_before}" -v total_after="${total_after}" '
            BEGIN { secs = elapsed_ns / 1e9 }
            FNR == NR { before[$1] = $2; next }
            ($1 in before) { printf "THREAD %s %s %.2f\n", $1, $3, ($2 - before[$1]) * 100 / clk / secs }
            END { printf "TOTAL %.2f\n", (total_after - total_before) * 100 / clk / secs }
        ' <(printf '%s\n' "${before}") <(printf '%s\n' "${after}") > "${CPU_RAW}"
    fi

    # ...and only up to here: the shutdown summary covers the SIGTERM handling, not idle.
    local log_end_line cpu_end
    log_end_line="$(wc -l < "${APP_LOG}")"
    cpu_end="$(date +%s.%3N)"

    echo "Closing TaskSmack..."
    stop_app
    if [[ "${APP_KILLED}" -eq 1 ]]; then
        die "TaskSmack did not exit within 10s of SIGTERM and was killed. See ${APP_LOG}."
    elif [[ "${APP_EXIT_CODE}" -ne 0 ]]; then
        die "TaskSmack exited with code ${APP_EXIT_CODE} after SIGTERM. See ${APP_LOG}."
    fi

    # pidstat's "Average:" block → the same THREAD/TOTAL rows. Columns are found by header name,
    # since sysstat versions differ in which ones they print.
    if [[ "${SAMPLER}" = "pidstat" ]]; then
        awk '
            /^Average:/ && /%CPU/ {
                for (i = 1; i <= NF; i++) { col[$i] = i }
                have = 1; next
            }
            have && /^Average:/ {
                tid = $col["TID"]; cpu = $col["%CPU"]; cmd = $col["Command"]
                if (tid == "-") { printf "TOTAL %s\n", cpu }
                else { sub(/^\|__/, "", cmd); printf "THREAD %s %s %s\n", tid, cmd, cpu }
            }
        ' "${CPU_RAW}" > "${CPU_RAW}.parsed"
        mv "${CPU_RAW}.parsed" "${CPU_RAW}"
        grep -q '^TOTAL ' "${CPU_RAW}" || die "Could not read pidstat's Average block."
    fi

    # ResizePerf summaries logged while sampling → fps, frame p95/p99/max, loop p95/p99.
    local frame_stats summaries fps frame_p95 frame_p99 frame_max loop_p95 loop_p99
    frame_stats="$(sed -n "$((log_start_line + 1)),${log_end_line}p" "${APP_LOG}" | awk '
        /ResizePerf\[/ {
            lines++
            for (i = 1; i <= NF; i++) {
                # loopIntervals=N precedes "loop avg/..." on the line: N weights that avg.
                if ($i ~ /^loopIntervals=/) { pending = substr($i, 15) + 0; intervals += pending }
                if ($i ~ /^avg\/p95\/p99\/max=/ && i > 1 && ($(i - 1) == "frame" || $(i - 1) == "loop")) {
                    split(substr($i, index($i, "=") + 1), v, "/")
                    if ($(i - 1) == "frame") {
                        if (v[2] > fp95) fp95 = v[2]; if (v[3] > fp99) fp99 = v[3]; if (v[4] > fmax) fmax = v[4]
                    } else {
                        loopms += v[1] * pending
                        if (v[2] > lp95) lp95 = v[2]; if (v[3] > lp99) lp99 = v[3]
                    }
                }
            }
        }
        END {
            fps = (loopms > 0) ? intervals * 1000 / loopms : 0
            printf "%d %.2f %.3f %.3f %.3f %.3f %.3f\n", lines, fps, fp95, fp99, fmax, lp95, lp99
        }
    ')"
    read -r summaries fps frame_p95 frame_p99 frame_max loop_p95 loop_p99 <<<"${frame_stats}"
    if [[ "${summaries}" -eq 0 ]]; then
        echo "WARNING: no ResizePerf summaries were logged while sampling (they are logged every 5 s at idle); frame figures are 0." >&2
    fi

    # The span the summaries cover: each periodic summary describes the time since the previous one
    # (or since tracing started with the main loop), so the span starts at the last summary logged
    # before sampling and ends at the last summary logged during it.
    local trace_start="" trace_end="" previous_summary trace_span
    if [[ "${summaries}" -gt 0 ]]; then
        previous_summary="$(head -n "${log_start_line}" "${APP_LOG}" | grep 'ResizePerf\[' | tail -n 1 || true)"
        [[ -n "${previous_summary}" ]] || previous_summary="$(grep -m 1 "${MAIN_LOOP_MARKER}" "${APP_LOG}" || true)"
        trace_start="$(log_line_epoch "${previous_summary}" || true)"
        trace_end="$(log_line_epoch "$(sed -n "$((log_start_line + 1)),${log_end_line}p" "${APP_LOG}" | grep 'ResizePerf\[' | tail -n 1)" || true)"
    fi
    trace_span="$(awk -v a="${trace_start:-0}" -v b="${trace_end:-0}" 'BEGIN { printf "%.1f", (a > 0 && b > a) ? b - a : 0 }')"

    # Per-thread rows with the Mesa flag, then total / Mesa / app CPU.
    local threads_file="${WORK_DIR}/threads-${rep}"
    awk -v re="${MESA_THREAD_REGEX}" '/^THREAD / { print $2, $3, $4, ($3 ~ re) ? 1 : 0 }' "${CPU_RAW}" \
        | sort -k3,3 -g -r > "${threads_file}"
    local total_cpu mesa_cpu app_cpu
    total_cpu="$(awk '/^TOTAL / { printf "%.2f", $2 }' "${CPU_RAW}")"
    mesa_cpu="$(awk '$4 == 1 { s += $3 } END { printf "%.2f", s }' "${threads_file}")"
    app_cpu="$(awk -v t="${total_cpu}" -v m="${mesa_cpu}" 'BEGIN { a = t - m; printf "%.2f", (a > 0) ? a : 0 }')"

    echo
    echo "TaskSmack idle measurement — label=${LABEL} preset=${PRESET} duration=${DURATION_SECONDS}s sampler=${SAMPLER} synthetic=${SYNTHETIC:-none} repetition=${rep}/${REPEAT}"
    if [[ "${IS_WSL}" -eq 1 ]]; then
        echo "NOTE: running under WSL — app CPU is evidence; total CPU is mostly llvmpipe, and frame-time figures are not representative of native Linux."
    fi
    echo
    printf '  %-8s %-16s %8s\n' "TID" "Thread" "CPU%"
    local tid name cpu mesa
    while read -r tid name cpu mesa; do
        printf '  %-8s %-16s %8.2f%s\n' "${tid}" "${name}" "${cpu}" "$([[ "${mesa}" -eq 1 ]] && echo "  (Mesa)")"
    done < "${threads_file}"
    printf '  %-8s %-16s %8.2f\n' "" "TOTAL" "${total_cpu}"
    printf '  %-8s %-16s %8.2f\n' "" "Mesa threads" "${mesa_cpu}"
    printf '  %-8s %-16s %8.2f\n' "" "APP (total-Mesa)" "${app_cpu}"
    echo
    printf '  %-28s %10s\n' "fps (presented)" "${fps}"
    printf '  %-28s %10s\n' "frame p95 / p99 / max (ms)" "${frame_p95} / ${frame_p99} / ${frame_max}"
    printf '  %-28s %10s\n' "loop p95 / p99 (ms)" "${loop_p95} / ${loop_p99}"
    printf '  %-28s %10s\n' "ResizePerf summaries" "${summaries}"
    echo
    echo "  CPU sample:             $(describe_span "${cpu_start}" "${cpu_end}")"
    echo "  Frame figures' span:    $(describe_span "${trace_start}" "${trace_end}")"
    echo "  (The app logs summaries on its own 5 s schedule, so the two spans differ by up to one"
    echo "   summary interval at each end.)"
    echo
    # One machine-readable line per run, for collecting a scenario matrix.
    echo "RESULT label=${LABEL} preset=${PRESET} rep=${rep}/${REPEAT} duration=${DURATION_SECONDS} sampler=${SAMPLER} totalCpu=${total_cpu} appCpu=${app_cpu} mesaCpu=${mesa_cpu} fps=${fps} frameP95=${frame_p95} frameP99=${frame_p99} frameMax=${frame_max} loopP95=${loop_p95} loopP99=${loop_p99} cpuStart=${cpu_start} cpuEnd=${cpu_end} traceStart=${trace_start:-0} traceEnd=${trace_end:-0} traceSpan=${trace_span} synthetic=${SYNTHETIC:-none}"

    echo "${rep} ${total_cpu} ${app_cpu} ${mesa_cpu} ${fps} ${frame_p95} ${frame_p99} ${frame_max} ${loop_p95} ${loop_p99} ${summaries} ${cpu_start} ${cpu_end} ${trace_start:-0} ${trace_end:-0} ${trace_span} ${APP_LOG}" >> "${REPS_FILE}"
}

if command -v pidstat &>/dev/null; then
    SAMPLER="pidstat"
else
    SAMPLER="/proc"
fi

for ((REP = 1; REP <= REPEAT; REP++)); do
    run_once "${REP}"
done

# "mean median p95" of column $1 of REPS_FILE (p95 by nearest rank).
aggregate() {
    awk -v c="$1" '{ print $c }' "${REPS_FILE}" | sort -g | awk '
        { v[NR] = $1; s += $1 }
        END {
            n = NR
            med = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
            r = int(0.95 * n); if (r < 0.95 * n) r++; if (r < 1) r = 1
            printf "%.2f %.2f %.2f %.2f %.2f\n", s / n, med, v[r], v[1], v[n]
        }'
}
read -r TOTAL_MEAN TOTAL_MEDIAN TOTAL_P95 TOTAL_MIN TOTAL_MAX <<<"$(aggregate 2)"
read -r APP_MEAN APP_MEDIAN APP_P95 APP_MIN APP_MAX <<<"$(aggregate 3)"
read -r FPS_MEAN FPS_MEDIAN FPS_P95 FPS_MIN FPS_MAX <<<"$(aggregate 5)"

# ---- Provenance ---------------------------------------------------------------------------------
FIRST_LOG="$(head -n 1 "${REPS_FILE}" | cut -d ' ' -f 17-)"

# A constant's value from SamplingConfig.h ("inline constexpr int NAME = 123;").
sampling_constant() {
    sed -n "s/^inline constexpr int $1 = \([0-9]*\);.*/\1/p" "${SAMPLING_CONFIG_H}" 2>/dev/null | head -n 1
}
# A [sampling] key's value from a config.toml.
config_sampling_value() {
    [[ -r "$1" ]] || return 0
    awk -v key="$2" '
        /^\[/ { in_sampling = ($0 == "[sampling]"); next }
        in_sampling && $1 == key && $2 == "=" { print $3; exit }
    ' "$1"
}

GIT_COMMIT="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || true)"
GIT_DIRTY=false
[[ -n "$(git -C "${REPO_ROOT}" status --porcelain --untracked-files=no 2>/dev/null || true)" ]] && GIT_DIRTY=true
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "${REPO_ROOT}/build/${PRESET}/CMakeCache.txt" 2>/dev/null | head -n 1)"

# The app logs where it loaded its config from ("Loaded config from <path>").
CONFIG_PATH="$(sed -n 's/.*Loaded config from \(.*\)$/\1/p' "${FIRST_LOG}" | head -n 1)"

# Refresh interval: the background samplers log the interval they run at (and any change).
REFRESH_MS="$(grep -oE 'BackgroundSampler: (starting with|interval changed to) [0-9]+ms' "${FIRST_LOG}" \
    | tail -n 1 | grep -oE '[0-9]+' || true)"
REFRESH_SOURCE="app log"
if [[ -z "${REFRESH_MS}" ]]; then
    REFRESH_MS="$(config_sampling_value "${CONFIG_PATH}" interval_ms)"
    REFRESH_SOURCE="config"
fi
if [[ -z "${REFRESH_MS}" ]]; then
    REFRESH_MS="$(sampling_constant REFRESH_INTERVAL_DEFAULT_MS)"
    REFRESH_SOURCE="SamplingConfig.h default"
fi

# History window: the synthetic scenario's (logged as history=<N>s) when it sets one, else the
# config file's, else SamplingConfig.h's default.
HISTORY_S="$(grep -m 1 'showing a synthetic machine' "${FIRST_LOG}" | grep -oE 'history=[0-9]+s' | grep -oE '[0-9]+' || true)"
HISTORY_SOURCE="synthetic scenario"
if [[ -z "${HISTORY_S}" || "${HISTORY_S}" -eq 0 ]]; then
    HISTORY_S="$(config_sampling_value "${CONFIG_PATH}" history_max_seconds)"
    HISTORY_SOURCE="config"
fi
if [[ -z "${HISTORY_S}" ]]; then
    HISTORY_S="$(sampling_constant HISTORY_SECONDS_DEFAULT)"
    HISTORY_SOURCE="SamplingConfig.h default"
fi

GL_RENDERER="$(sed -n 's/.*  Renderer: \(.*\)$/\1/p' "${FIRST_LOG}" | head -n 1)"

# Display refresh: the current mode of the primary output (else the first connected one) in xrandr.
DISPLAY_HZ=""
if [[ -n "${DISPLAY:-}" ]] && command -v xrandr &>/dev/null; then
    DISPLAY_HZ="$(xrandr --current 2>/dev/null | awk '
        / connected/ { primary = ($3 == "primary"); next }
        /\*/ { for (i = 2; i <= NF; i++) if ($i ~ /\*/) { r = $i; gsub(/[*+]/, "", r)
                   if (primary && p == "") p = r; if (f == "") f = r } }
        END { print (p != "") ? p : f }' || true)"
fi

CPU_MODEL="$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -n 1)"
[[ -n "${CPU_MODEL}" ]] || CPU_MODEL="$(LC_ALL=C lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -n 1)"
LOGICAL_CPUS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)"
CPU_MHZ="$(awk -F: '/^cpu MHz/ { s += $2; n++ } END { if (n) printf "%.0f", s / n }' /proc/cpuinfo)"
CPU_MAX_MHZ="$(LC_ALL=C lscpu 2>/dev/null | sed -n 's/^CPU max MHz:[[:space:]]*//p' | head -n 1)"
KERNEL="$(uname -r)"

# ---- Summary ------------------------------------------------------------------------------------
echo
echo "=== Summary over ${REPEAT} repetition(s): label=${LABEL} preset=${PRESET} synthetic=${SYNTHETIC:-none}"
printf '  %-14s %8s %8s %8s %8s %8s\n' "" "mean" "median" "p95" "min" "max"
printf '  %-14s %8s %8s %8s %8s %8s\n' "app CPU%" "${APP_MEAN}" "${APP_MEDIAN}" "${APP_P95}" "${APP_MIN}" "${APP_MAX}"
printf '  %-14s %8s %8s %8s %8s %8s\n' "total CPU%" "${TOTAL_MEAN}" "${TOTAL_MEDIAN}" "${TOTAL_P95}" "${TOTAL_MIN}" "${TOTAL_MAX}"
printf '  %-14s %8s %8s %8s %8s %8s\n' "fps" "${FPS_MEAN}" "${FPS_MEDIAN}" "${FPS_P95}" "${FPS_MIN}" "${FPS_MAX}"
info "refresh=${REFRESH_MS}ms (${REFRESH_SOURCE}) history=${HISTORY_S}s (${HISTORY_SOURCE}) display=${DISPLAY_HZ:-unknown}Hz renderer=${GL_RENDERER:-unknown}"
echo "SUMMARY label=${LABEL} preset=${PRESET} reps=${REPEAT} appCpuMean=${APP_MEAN} appCpuMedian=${APP_MEDIAN} appCpuP95=${APP_P95} totalCpuMean=${TOTAL_MEAN} totalCpuMedian=${TOTAL_MEDIAN} totalCpuP95=${TOTAL_P95} fpsMean=${FPS_MEAN} fpsMedian=${FPS_MEDIAN} fpsP95=${FPS_P95} synthetic=${SYNTHETIC:-none}"

FAILED=false
if [[ -n "${FAIL_ABOVE}" ]] && awk -v m="${APP_MEDIAN}" -v t="${FAIL_ABOVE}" 'BEGIN { exit !(m > t) }'; then
    FAILED=true
fi

# ---- JSON ---------------------------------------------------------------------------------------
# A JSON string literal (or null for an empty value).
json_str() {
    if [[ -z "$1" ]]; then
        printf 'null'
        return
    fi
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="$(printf '%s' "${s}" | tr -d '\000-\037')"
    printf '"%s"' "${s}"
}
# A JSON number (or null for an empty value).
json_num() {
    if [[ -z "$1" ]]; then printf 'null'; else printf '%s' "$1"; fi
}
json_stats() { printf '{ "mean": %s, "median": %s, "p95": %s, "min": %s, "max": %s }' "$@"; }

{
    printf '{\n'
    printf '  "schema": "tasksmack-idle/1",\n'
    printf '  "label": %s,\n' "$(json_str "${LABEL}")"
    printf '  "timestamp": %s,\n' "$(json_str "$(date -u +%Y-%m-%dT%H:%M:%SZ)")"
    printf '  "git": { "commit": %s, "dirty": %s },\n' "$(json_str "${GIT_COMMIT}")" "${GIT_DIRTY}"
    printf '  "build": { "preset": %s, "buildType": %s },\n' "$(json_str "${PRESET}")" "$(json_str "${BUILD_TYPE}")"
    printf '  "scenario": {\n'
    printf '    "synthetic": %s,\n' "$(json_str "${SYNTHETIC}")"
    printf '    "setupCmd": %s,\n' "$(json_str "${SETUP_CMD}")"
    printf '    "refreshIntervalMs": %s, "refreshIntervalSource": %s,\n' "$(json_num "${REFRESH_MS}")" "$(json_str "${REFRESH_SOURCE}")"
    printf '    "historySeconds": %s, "historySource": %s,\n' "$(json_num "${HISTORY_S}")" "$(json_str "${HISTORY_SOURCE}")"
    printf '    "warmupSeconds": %s, "durationSeconds": %s, "repeat": %s\n' "${WARMUP_SECONDS}" "${DURATION_SECONDS}" "${REPEAT}"
    printf '  },\n'
    printf '  "machine": {\n'
    printf '    "cpuModel": %s, "logicalCpus": %s, "cpuMhz": %s, "cpuMaxMhz": %s,\n' \
        "$(json_str "${CPU_MODEL}")" "$(json_num "${LOGICAL_CPUS}")" "$(json_num "${CPU_MHZ}")" "$(json_num "${CPU_MAX_MHZ}")"
    printf '    "kernel": %s, "wsl": %s, "displayRefreshHz": %s, "glRenderer": %s\n' \
        "$(json_str "${KERNEL}")" "$([[ "${IS_WSL}" -eq 1 ]] && echo true || echo false)" \
        "$(json_num "${DISPLAY_HZ}")" "$(json_str "${GL_RENDERER}")"
    printf '  },\n'
    printf '  "sampler": %s,\n' "$(json_str "${SAMPLER}")"
    printf '  "mesaThreadRegex": %s,\n' "$(json_str "${MESA_THREAD_REGEX}")"
    printf '  "repetitions": [\n'
    REP_COUNT=0
    while read -r rep total app mesa fps fp95 fp99 fmax lp95 lp99 summaries cstart cend tstart tend tspan applog; do
        REP_COUNT=$((REP_COUNT + 1))
        printf '    {\n'
        printf '      "rep": %s, "totalCpu": %s, "appCpu": %s, "mesaCpu": %s, "fps": %s,\n' "${rep}" "${total}" "${app}" "${mesa}" "${fps}"
        printf '      "frameP95Ms": %s, "frameP99Ms": %s, "frameMaxMs": %s, "loopP95Ms": %s, "loopP99Ms": %s,\n' \
            "${fp95}" "${fp99}" "${fmax}" "${lp95}" "${lp99}"
        printf '      "resizePerfSummaries": %s, "cpuStart": %s, "cpuEnd": %s, "traceStart": %s, "traceEnd": %s, "traceSpanSeconds": %s,\n' \
            "${summaries}" "${cstart}" "${cend}" "${tstart}" "${tend}" "${tspan}"
        printf '      "appLog": %s,\n' "$(json_str "${applog#"${REPO_ROOT}"/}")"
        printf '      "threads": ['
        first=1
        while read -r tid name cpu mesa_flag; do
            if [[ "${first}" -eq 1 ]]; then printf '\n'; else printf ',\n'; fi
            first=0
            printf '        { "tid": %s, "name": %s, "cpu": %s, "mesa": %s }' "${tid}" "$(json_str "${name}")" "${cpu}" \
                "$([[ "${mesa_flag}" -eq 1 ]] && echo true || echo false)"
        done < "${WORK_DIR}/threads-${rep}"
        printf '\n      ]\n'
        printf '    }%s\n' "$([[ "${REP_COUNT}" -lt "${REPEAT}" ]] && echo ",")"
    done < "${REPS_FILE}"
    printf '  ],\n'
    printf '  "aggregates": {\n'
    printf '    "appCpu": %s,\n' "$(json_stats "${APP_MEAN}" "${APP_MEDIAN}" "${APP_P95}" "${APP_MIN}" "${APP_MAX}")"
    printf '    "totalCpu": %s,\n' "$(json_stats "${TOTAL_MEAN}" "${TOTAL_MEDIAN}" "${TOTAL_P95}" "${TOTAL_MIN}" "${TOTAL_MAX}")"
    printf '    "fps": %s\n' "$(json_stats "${FPS_MEAN}" "${FPS_MEDIAN}" "${FPS_P95}" "${FPS_MIN}" "${FPS_MAX}")"
    printf '  },\n'
    printf '  "failAbove": %s, "failed": %s\n' "$(json_num "${FAIL_ABOVE}")" "${FAILED}"
    printf '}\n'
} > "${JSON_PATH}"
info "JSON:    ${JSON_PATH}"

if [[ "${FAILED}" = true ]]; then
    echo "FAIL: median app CPU ${APP_MEDIAN}% is above --fail-above ${FAIL_ABOVE}%." >&2
    exit 3
elif [[ -n "${FAIL_ABOVE}" ]]; then
    echo "PASS: median app CPU ${APP_MEDIAN}% is within --fail-above ${FAIL_ABOVE}%."
fi
