#!/usr/bin/env bash
# tools/measure-idle.sh — Measure TaskSmack's idle CPU% (per thread) and frame time on Linux.
#
# Usage:
#   ./tools/measure-idle.sh [--preset <preset>] [--skip-build] [--warmup <seconds>]
#                           [--duration <seconds>] [--label <name>] [--setup-cmd <command>]
#                           [--synthetic <spec>]
#
# Launches TaskSmack with TASKSMACK_TRACE_RESIZE_PERF=1, waits for its main loop and a warm-up (as
# tools/profile-perf.sh app mode does), samples per-thread CPU for --duration seconds, closes it
# with SIGTERM, then prints one table:
#   - per-thread average CPU% (100% = one logical CPU fully busy), and the process total;
#   - fps: presented frames per second, from the deliver-to-deliver loop intervals in the
#     ResizePerf summaries;
#   - frame p95/p99/max: work per presented frame (update+render+post+swap);
#   - loop p95/p99: deliver-to-deliver interval (frame end to frame end, skipped renders included).
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
#                         gets TASKSMACK_PID in its environment. Fails the run if it fails.
#   --synthetic <spec>    Measure the synthetic large-UI scenario instead of this machine: launch
#                         TaskSmack with TASKSMACK_SYNTHETIC=<spec>, e.g. processes=5000,history=full
#                         (keys: processes, cores, disks, interfaces, seed, history, refresh; see
#                         src/App/SyntheticScenario.h). A TASKSMACK_SYNTHETIC already in the
#                         environment is passed through too; this flag overrides it, and
#                         --synthetic '' turns an inherited one off. The spec is
#                         printed with the results and in the RESULT line as synthetic=<spec>.
#   -h, --help            Show this help.
#
# CPU sampling uses `pidstat -u -t -p <pid> 1 <duration>` (sysstat) when installed, otherwise the
# utime+stime deltas of /proc/<pid>/task/*/stat over the same window. Threads that start or exit
# during the window are left out of the per-thread rows; the total always counts them.
#
# TaskSmack's own output goes to perf-data/idle-<label>-<timestamp>-app.log. The run fails if
# TaskSmack exits early, exits non-zero after SIGTERM, or needs SIGKILL.
#
# On WSL (WSLg), the CPU figures are valid evidence; frame-time figures are not representative of
# a native compositor/GPU driver. See CONTRIBUTING.md, "Measuring idle CPU and frame time".
#
# Examples:
#   ./tools/measure-idle.sh
#   ./tools/measure-idle.sh --preset debug --skip-build --duration 30 --label overview
#   ./tools/measure-idle.sh --skip-build --label processes --setup-cmd 'sleep 2; xdotool mousemove <x> <y> click 1'
#   ./tools/measure-idle.sh --preset debug --skip-build --label synthetic-overview --synthetic processes=5000,history=full

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

PERF_DIR="${REPO_ROOT}/perf-data"

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

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)    [[ $# -ge 2 ]] || die "$1 requires a value"; PRESET="$2";           shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --warmup)    [[ $# -ge 2 ]] || die "$1 requires a value"; WARMUP_SECONDS="$2";   shift 2 ;;
        --duration)  [[ $# -ge 2 ]] || die "$1 requires a value"; DURATION_SECONDS="$2"; shift 2 ;;
        --label)     [[ $# -ge 2 ]] || die "$1 requires a value"; LABEL="$2";            shift 2 ;;
        --setup-cmd) [[ $# -ge 2 ]] || die "$1 requires a value"; SETUP_CMD="$2";        shift 2 ;;
        --synthetic) [[ $# -ge 2 ]] || die "$1 requires a value"; SYNTHETIC="$2";        shift 2 ;;
        -h|--help)   usage; exit 0 ;;
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
# Base 10: Bash reads a leading zero as octal in -gt and $(( )), so "08" would abort.
WARMUP_SECONDS=$((10#${WARMUP_SECONDS}))
DURATION_SECONDS=$((10#${DURATION_SECONDS}))
[[ "${DURATION_SECONDS}" -gt 0 ]] || die "--duration must be at least 1 second"
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
APP_LOG="${PERF_DIR}/idle-${LABEL}-${TIMESTAMP}-app.log"
CPU_RAW="$(mktemp)"

MAIN_LOOP_TIMEOUT_SECONDS=30
MAIN_LOOP_MARKER="Entering main loop" # Logged by Core::Application::run() at info level
APP_PID=""
APP_EXIT_CODE=""
APP_KILLED=0

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
    rm -f "${CPU_RAW}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check_alive() {
    if ! app_alive; then
        reap_app
        die "TaskSmack exited (code ${APP_EXIT_CODE}) $1. See ${APP_LOG}."
    fi
}

echo "Measuring preset=${PRESET} label=${LABEL} warmup=${WARMUP_SECONDS}s duration=${DURATION_SECONDS}s synthetic=${SYNTHETIC:-none}"
info "Binary:  ${BINARY}"
info "App log: ${APP_LOG}"

APP_ENV=(TASKSMACK_TRACE_RESIZE_PERF=1 "TASKSMACK_LOG_LEVEL=${TASKSMACK_LOG_LEVEL:-info}")
# Always set, even when empty: an explicit --synthetic '' must override an inherited
# TASKSMACK_SYNTHETIC (an empty value turns the scenario off), so the run matches what is reported.
APP_ENV+=("TASKSMACK_SYNTHETIC=${SYNTHETIC}")
# env execs TaskSmack in place, so $! is TaskSmack's own PID.
env "${APP_ENV[@]}" "${BINARY}" > "${APP_LOG}" 2>&1 &
APP_PID=$!

echo "Launched TaskSmack (pid ${APP_PID}). Waiting for its main loop (up to ${MAIN_LOOP_TIMEOUT_SECONDS}s)..."
MAIN_LOOP_DEADLINE=$((SECONDS + MAIN_LOOP_TIMEOUT_SECONDS))
MAIN_LOOP_SEEN=0
while ((SECONDS < MAIN_LOOP_DEADLINE)); do
    check_alive "before its main loop started"
    if grep -q "${MAIN_LOOP_MARKER}" "${APP_LOG}" 2>/dev/null; then
        MAIN_LOOP_SEEN=1
        break
    fi
    sleep 0.25
done
check_alive "before its main loop started"
if [[ "${MAIN_LOOP_SEEN}" -eq 0 ]]; then
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
LOG_START_LINE="$(wc -l < "${APP_LOG}")"
CPU_START="$(date +%s.%3N)"
CLK_TCK="$(getconf CLK_TCK)"

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

SAMPLER=""
if command -v pidstat &>/dev/null; then
    SAMPLER="pidstat"
    echo "Sampling per-thread CPU with pidstat for ${DURATION_SECONDS}s..."
    LC_ALL=C pidstat -u -t -p "${APP_PID}" 1 "${DURATION_SECONDS}" > "${CPU_RAW}" 2>&1 \
        || { check_alive "while sampling"; die "pidstat failed: $(tail -n 3 "${CPU_RAW}")"; }
    check_alive "while sampling"
else
    SAMPLER="/proc"
    echo "pidstat not found (install sysstat for it); sampling /proc/${APP_PID}/task/*/stat for ${DURATION_SECONDS}s..."
    BEFORE="$(snapshot_threads)"
    TOTAL_BEFORE="$(stat_ticks "/proc/${APP_PID}/stat")"
    START_NS="$(date +%s%N)"
    for ((i = 0; i < DURATION_SECONDS; i++)); do
        check_alive "while sampling"
        sleep 1
    done
    check_alive "while sampling"
    AFTER="$(snapshot_threads)"
    TOTAL_AFTER="$(stat_ticks "/proc/${APP_PID}/stat")"
    END_NS="$(date +%s%N)"
    # Normalised to the pidstat layout the report below reads: "tid name cpu%" rows plus a total.
    awk -v clk="${CLK_TCK}" -v elapsed_ns="$((END_NS - START_NS))" \
        -v total_before="${TOTAL_BEFORE}" -v total_after="${TOTAL_AFTER}" '
        BEGIN { secs = elapsed_ns / 1e9 }
        FNR == NR { before[$1] = $2; next }
        ($1 in before) { printf "THREAD %s %s %.2f\n", $1, $3, ($2 - before[$1]) * 100 / clk / secs }
        END { printf "TOTAL %.2f\n", (total_after - total_before) * 100 / clk / secs }
    ' <(printf '%s\n' "${BEFORE}") <(printf '%s\n' "${AFTER}") > "${CPU_RAW}"
fi

# ...and only up to here: the shutdown summary covers the SIGTERM handling, not idle.
LOG_END_LINE="$(wc -l < "${APP_LOG}")"
CPU_END="$(date +%s.%3N)"

echo "Closing TaskSmack..."
stop_app
if [[ "${APP_KILLED}" -eq 1 ]]; then
    die "TaskSmack did not exit within 10s of SIGTERM and was killed. See ${APP_LOG}."
elif [[ "${APP_EXIT_CODE}" -ne 0 ]]; then
    die "TaskSmack exited with code ${APP_EXIT_CODE} after SIGTERM. See ${APP_LOG}."
fi

# pidstat's "Average:" block → the same THREAD/TOTAL rows. Columns are found by header name, since
# sysstat versions differ in which ones they print.
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
FRAME_STATS="$(sed -n "$((LOG_START_LINE + 1)),${LOG_END_LINE}p" "${APP_LOG}" | awk '
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
read -r SUMMARIES FPS FRAME_P95 FRAME_P99 FRAME_MAX LOOP_P95 LOOP_P99 <<<"${FRAME_STATS}"
if [[ "${SUMMARIES}" -eq 0 ]]; then
    echo "WARNING: no ResizePerf summaries were logged while sampling (they are logged every 5 s at idle); frame figures are 0." >&2
fi

# Epoch seconds of a spdlog line ("[2026-10-06 15:13:32.952] ..."); empty if it has none.
log_line_epoch() {
    local stamp
    stamp="$(sed -n 's/^\[\([0-9-]* [0-9:.]*\)\].*/\1/p' <<<"$1")"
    [[ -n "${stamp}" ]] && date -d "${stamp}" +%s.%3N
}
# The span the summaries cover: each periodic summary describes the time since the previous one
# (or since tracing started with the main loop), so the span starts at the last summary logged
# before sampling and ends at the last summary logged during it.
TRACE_START=""
TRACE_END=""
if [[ "${SUMMARIES}" -gt 0 ]]; then
    PREVIOUS_SUMMARY="$(head -n "${LOG_START_LINE}" "${APP_LOG}" | grep 'ResizePerf\[' | tail -n 1 || true)"
    [[ -n "${PREVIOUS_SUMMARY}" ]] || PREVIOUS_SUMMARY="$(grep -m 1 "${MAIN_LOOP_MARKER}" "${APP_LOG}" || true)"
    TRACE_START="$(log_line_epoch "${PREVIOUS_SUMMARY}" || true)"
    TRACE_END="$(log_line_epoch "$(sed -n "$((LOG_START_LINE + 1)),${LOG_END_LINE}p" "${APP_LOG}" | grep 'ResizePerf\[' | tail -n 1)" || true)"
fi
# "HH:MM:SS.mmm → HH:MM:SS.mmm (N.N s)" for two epoch times, or "n/a".
describe_span() {
    if [[ -z "$1" || -z "$2" ]]; then
        echo "n/a"
        return
    fi
    printf '%s → %s (%.1f s)' "$(date -d "@$1" +%H:%M:%S.%3N)" "$(date -d "@$2" +%H:%M:%S.%3N)" \
        "$(awk -v a="$1" -v b="$2" 'BEGIN { print b - a }')"
}
TRACE_SPAN_SECONDS="$(awk -v a="${TRACE_START:-0}" -v b="${TRACE_END:-0}" 'BEGIN { printf "%.1f", (a > 0 && b > a) ? b - a : 0 }')"

TOTAL_CPU="$(awk '/^TOTAL / { print $2 }' "${CPU_RAW}")"

echo
echo "TaskSmack idle measurement — label=${LABEL} preset=${PRESET} duration=${DURATION_SECONDS}s sampler=${SAMPLER} synthetic=${SYNTHETIC:-none}"
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    echo "NOTE: running under WSL — CPU figures are evidence; frame-time figures are not representative of native Linux."
fi
echo
printf '  %-8s %-16s %8s\n' "TID" "Thread" "CPU%"
awk '/^THREAD / { print $2, $3, $4 }' "${CPU_RAW}" | sort -k3,3 -g -r | while read -r tid name cpu; do
    printf '  %-8s %-16s %8.2f\n' "${tid}" "${name}" "${cpu}"
done
printf '  %-8s %-16s %8.2f\n' "" "TOTAL" "${TOTAL_CPU}"
echo
printf '  %-28s %10s\n' "fps (presented)" "${FPS}"
printf '  %-28s %10s\n' "frame p95 / p99 / max (ms)" "${FRAME_P95} / ${FRAME_P99} / ${FRAME_MAX}"
printf '  %-28s %10s\n' "loop p95 / p99 (ms)" "${LOOP_P95} / ${LOOP_P99}"
printf '  %-28s %10s\n' "ResizePerf summaries" "${SUMMARIES}"
echo
echo "  CPU sample:             $(describe_span "${CPU_START}" "${CPU_END}")"
echo "  Frame figures' span:    $(describe_span "${TRACE_START}" "${TRACE_END}")"
echo "  (The app logs summaries on its own 5 s schedule, so the two spans differ by up to one"
echo "   summary interval at each end.)"
echo
# One machine-readable line per run, for collecting a scenario matrix.
echo "RESULT label=${LABEL} preset=${PRESET} duration=${DURATION_SECONDS} sampler=${SAMPLER} totalCpu=${TOTAL_CPU} fps=${FPS} frameP95=${FRAME_P95} frameP99=${FRAME_P99} frameMax=${FRAME_MAX} loopP95=${LOOP_P95} loopP99=${LOOP_P99} cpuStart=${CPU_START} cpuEnd=${CPU_END} traceStart=${TRACE_START:-0} traceEnd=${TRACE_END:-0} traceSpan=${TRACE_SPAN_SECONDS} synthetic=${SYNTHETIC:-none}"
