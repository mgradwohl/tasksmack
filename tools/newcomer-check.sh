#!/usr/bin/env bash
# tools/newcomer-check.sh — Follow the documented Linux newcomer path end to end (#2618).
#
# Usage:
#   ./tools/newcomer-check.sh [--repo <path-or-url>] [--ref <sha-or-branch>] [--jobs <N>]
#                             [--workdir <dir>] [--perf-duration <seconds>] [--full-bench]
#                             [--keep-going] [--stop-on-finding]
#   ./tools/newcomer-check.sh -h | --help
#
# Meant for a fresh Ubuntu 24.04 WSL distro with none of TaskSmack's tools installed, e.g.
#   wsl --install Ubuntu-24.04 --name tasksmack-newcomer
# Copy only this script into the distro and run it there; it clones the repository itself. It
# installs packages with sudo (it asks for your password once, up front) and does several full
# builds, so expect it to take an hour or more.
#
# It does ONLY what README.md and then CONTRIBUTING.md tell a Linux newcomer to do, in their order:
#   1. preflight        OS, kernel, display and sudo check
#   2. git              install git if missing (no doc says to: a FINDING if it was needed)
#   3. clone            git clone (CONTRIBUTING "Quick Start"), then check out --ref
#   4. readme           README.md leads a newcomer to the build steps
#   5. quickstart       CONTRIBUTING "Quick Start": python3.14 venv, requirements.txt, pre-commit
#   6. setup-dev        ./tools/setup-dev.sh ("Automated Setup (Linux)")
#   7. activate         source .venv/bin/activate (and retry the Quick Start Python steps when
#                       they could not run before setup)
#   8. check-prereqs    ./tools/check-prereqs.sh
#   9. dev-workflow     cmake --workflow --preset dev ("One-Command Dev Workflow")
#  10. ctest            ctest --preset debug ("Test")
#  11. smoke-launch     build/debug/bin/TaskSmack ("Running"): no clicking. TASKSMACK_CONFIG_DIR is a
#                       fresh temporary directory; the window must log "Entering main loop" and then a
#                       ResizePerf frame summary (frames presented), and exit 0 after SIGTERM
#  12. benchmark-build  cmake --preset benchmark / cmake --build --preset benchmark ("Benchmarks")
#  13. bench            ./tools/bench.sh benchmark, shortened unless --full-bench (see below)
#  14. measure-idle     ./tools/measure-idle.sh ("Measuring idle CPU and frame time"), which builds
#                       the profile preset first, sampling for --perf-duration seconds
#  15. profile-perf     ./tools/profile-perf.sh app --warmup 10 --duration <--perf-duration>
#                       ("Linux — CPU profiling"); SKIP when perf is not installed, since the docs
#                       leave installing perf as a manual step
#
# Every command a step runs is first looked up in the doc it comes from; the step log prints the
# doc's file:line, and a command the doc no longer contains is a FINDING. Results:
#   PASS     the step did what the docs say
#   FINDING  it worked, but the docs were wrong, ambiguous or incomplete (the notes say how)
#   FAIL     the docs' command failed; the run stops here unless --keep-going
#   SKIP     a precondition the docs leave optional or manual was not met (the notes say which)
#
# Options:
#   --repo <path-or-url>   Repository to clone. Default: https://github.com/mgradwohl/tasksmack.git
#   --ref <sha-or-branch>  Commit or branch to check out after cloning. Default: main.
#   --jobs <N>             Build parallelism for every CMake build (exported as
#                          CMAKE_BUILD_PARALLEL_LEVEL, since `cmake --workflow` takes no -j).
#                          Default: 4, not every core: a WSL VM runs out of memory otherwise.
#   --workdir <dir>        Where the clone, the step logs and results.md go. Must not exist yet, or
#                          be empty. Default: $HOME/newcomer-check-<timestamp>.
#   --perf-duration <s>    Sample/record length for measure-idle.sh and profile-perf.sh. Default: 30.
#   --full-bench           Run ./tools/bench.sh with its own flags (10 repetitions, 0.5 s minimum
#                          per benchmark). Default: add --benchmark_repetitions=1
#                          --benchmark_min_time=0.05s so the step proves the script works, quickly.
#   --keep-going           Continue after a FAIL (later steps may fail because of it).
#   --stop-on-finding      Stop at the first FINDING as well as at a FAIL.
#   -h, --help             Show this help.
#
# The app and the measure/profile scripts use the desktop when $WAYLAND_DISPLAY or $DISPLAY is set
# (WSLg); otherwise they run under `xvfb-run -a`, which tools/setup-dev.sh installs.
#
# At the end it prints a Markdown results table (step, result, duration, notes) with the commit
# tested, OS, kernel and date, ready to paste into #2618; the same table is in <workdir>/results.md
# and each step's full output in <workdir>/logs/. Exit status: 0 when nothing FAILed, 1 otherwise,
# 2 for a usage error.

set -euo pipefail

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die() { echo "ERROR: $*" >&2; exit 1; }
usage_error() { echo "ERROR: $*" >&2; echo "Run with --help for usage." >&2; exit 2; }

REPO_URL="https://github.com/mgradwohl/tasksmack.git"
REF="main"
JOBS=4
WORKDIR=""
PERF_DURATION=30
FULL_BENCH=0
KEEP_GOING=0
STOP_ON_FINDING=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)            [[ $# -ge 2 ]] || usage_error "$1 requires a value"; REPO_URL="$2";      shift 2 ;;
        --ref)             [[ $# -ge 2 ]] || usage_error "$1 requires a value"; REF="$2";           shift 2 ;;
        --jobs)            [[ $# -ge 2 ]] || usage_error "$1 requires a value"; JOBS="$2";          shift 2 ;;
        --workdir)         [[ $# -ge 2 ]] || usage_error "$1 requires a value"; WORKDIR="$2";       shift 2 ;;
        --perf-duration)   [[ $# -ge 2 ]] || usage_error "$1 requires a value"; PERF_DURATION="$2"; shift 2 ;;
        --full-bench)      FULL_BENCH=1; shift ;;
        --keep-going)      KEEP_GOING=1; shift ;;
        --stop-on-finding) STOP_ON_FINDING=1; shift ;;
        -h|--help)         usage; exit 0 ;;
        *)                 usage_error "Unknown argument: $1" ;;
    esac
done

[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || usage_error "--jobs must be a positive whole number"
[[ "${PERF_DURATION}" =~ ^[1-9][0-9]*$ ]] || usage_error "--perf-duration must be a positive whole number of seconds"
[[ -n "${REPO_URL}" ]] || usage_error "--repo must not be empty"
[[ -n "${REF}" ]] || usage_error "--ref must not be empty"
[[ "$(uname -s)" == "Linux" ]] || die "This check is for Linux (a fresh Ubuntu 24.04 WSL distro)."
[[ "${EUID}" -ne 0 ]] || die "Run as the distro's normal user, not root: a newcomer uses sudo where the docs say so."

[[ -n "${WORKDIR}" ]] || WORKDIR="${HOME}/newcomer-check-$(date +%Y%m%d-%H%M%S)"
if [[ -e "${WORKDIR}" ]] && [[ -n "$(ls -A "${WORKDIR}" 2>/dev/null)" ]]; then
    die "--workdir ${WORKDIR} already exists and is not empty; pick a new one."
fi
mkdir -p "${WORKDIR}/logs"
WORKDIR="$(cd "${WORKDIR}" && pwd)"
LOG_DIR="${WORKDIR}/logs"
CLONE_DIR="${WORKDIR}/tasksmack"
RESULTS_FILE="${WORKDIR}/results.md"

# Every CMake build (the workflow preset, the benchmark preset and the ones the tools/ scripts run)
# reads this when its build preset sets no jobs, which none of TaskSmack's do.
export CMAKE_BUILD_PARALLEL_LEVEL="${JOBS}"

# ---------- step bookkeeping ---------------------------------------------------------------------

STEP_INDEX=0
STEP_IDS=()
STEP_RESULTS=()
STEP_DURATIONS=()
STEP_NOTES=()
STOPPED=0          # 1 once a FAIL (or a FINDING with --stop-on-finding) stops the run
ANY_FAIL=0
STEP_STATUS=""     # set by the helpers below while a step runs
STEP_NOTE_LIST=()

COMMIT="(not cloned)"
DISPLAY_MODE=""
GUI_WRAP=()        # empty on a desktop, (xvfb-run -a) headless
QUICKSTART_DEFERRED=0
SUDO_KEEPALIVE_PID=""

note() {
    echo "NOTE: $*"
    STEP_NOTE_LIST+=("$*")
}
finding() {
    echo "FINDING: $*"
    [[ "${STEP_STATUS}" == "FAIL" ]] || STEP_STATUS="FINDING"
    STEP_NOTE_LIST+=("FINDING: $*")
}
# Records why the step failed; the caller then returns 1.
fail() {
    echo "FAIL: $*" >&2
    STEP_STATUS="FAIL"
    STEP_NOTE_LIST+=("$*")
}
skip() {
    echo "SKIP: $*"
    STEP_STATUS="SKIP"
    STEP_NOTE_LIST+=("$*")
}

# Prints "FILE:LINE" for the first line of the clone's FILE that contains TEXT, or nothing.
doc_line() {
    local file="$1" text="$2" n
    n="$(grep -nF -m1 -- "${text}" "${CLONE_DIR}/${file}" 2>/dev/null | cut -d: -f1)" || true
    [[ -n "${n}" ]] && printf '%s:%s' "${file}" "${n}"
    return 0
}
# Logs where the docs say to run TEXT; a doc that no longer says it is a FINDING (the check still
# runs the command it was written for).
cite() {
    local file="$1" text="$2" ref
    ref="$(doc_line "${file}" "${text}")"
    if [[ -n "${ref}" ]]; then
        echo "Doc: ${ref}: $(sed -n "${ref##*:}p" "${CLONE_DIR}/${file}" | sed 's/^[[:space:]]*//')"
    else
        finding "${file} no longer contains \`${text}\`; this check runs it as an older version of the doc said. Update tools/newcomer-check.sh or the doc."
    fi
}
# Echoes a command, then runs it.
run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

format_duration() {
    local s="$1"
    if ((s >= 60)); then
        printf '%dm %02ds' $((s / 60)) $((s % 60))
    else
        printf '%ds' "${s}"
    fi
}

# run_step <id> <function>: runs the function in this shell (so a step can set state for later
# ones), tees its output to logs/NN-<id>.log, times it and records PASS/FINDING/FAIL/SKIP.
run_step() {
    local id="$1" fn="$2"
    STEP_INDEX=$((STEP_INDEX + 1))
    local log
    log="${LOG_DIR}/$(printf '%02d' "${STEP_INDEX}")-${id}.log"

    if [[ "${STOPPED}" -eq 1 ]]; then
        STEP_IDS+=("${id}")
        STEP_RESULTS+=("NOT RUN")
        STEP_DURATIONS+=("-")
        STEP_NOTES+=("an earlier step stopped the run")
        return 0
    fi

    STEP_STATUS="PASS"
    STEP_NOTE_LIST=()
    echo
    echo "==================================================================================="
    echo "== Step ${STEP_INDEX}: ${id}   (log: ${log})"
    echo "==================================================================================="
    local start="${SECONDS}" rc=0
    # Not in a subshell: the step's variables (the commit, the activated venv) must survive it.
    if "${fn}" > >(tee -a "${log}") 2>&1; then rc=0; else rc=$?; fi
    local elapsed=$((SECONDS - start))
    if [[ "${rc}" -ne 0 && "${STEP_STATUS}" != "FAIL" ]]; then
        STEP_STATUS="FAIL"
        STEP_NOTE_LIST+=("exited with status ${rc}")
    fi

    local notes=""
    if [[ ${#STEP_NOTE_LIST[@]} -gt 0 ]]; then
        local IFS=';'
        notes="${STEP_NOTE_LIST[*]}"
        notes="${notes//;/; }"
    fi
    STEP_IDS+=("${id}")
    STEP_RESULTS+=("${STEP_STATUS}")
    STEP_DURATIONS+=("$(format_duration "${elapsed}")")
    STEP_NOTES+=("${notes}")
    echo "== ${id}: ${STEP_STATUS} in $(format_duration "${elapsed}")" | tee -a "${log}"

    if [[ "${STEP_STATUS}" == "FAIL" ]]; then
        ANY_FAIL=1
        if [[ "${KEEP_GOING}" -eq 0 ]]; then
            STOPPED=1
            echo "Stopping at the first FAIL (pass --keep-going to continue). See ${log}." >&2
        fi
    elif [[ "${STEP_STATUS}" == "FINDING" && "${STOP_ON_FINDING}" -eq 1 ]]; then
        STOPPED=1
        echo "Stopping at the first FINDING (--stop-on-finding). See ${log}." >&2
    fi
    return 0
}

# Markdown table cells: no raw pipes or newlines.
md_cell() {
    local s="${1//|/\\|}"
    printf '%s' "${s//$'\n'/ }"
}

write_results() {
    local os_name kernel
    # shellcheck disable=SC1091
    os_name="$( (. /etc/os-release && printf '%s' "${PRETTY_NAME:-${NAME:-unknown}}") 2>/dev/null || echo unknown)"
    kernel="$(uname -r)"
    {
        echo "### Linux newcomer check (tools/newcomer-check.sh)"
        echo
        echo "- Commit: \`${COMMIT}\` (repo \`${REPO_URL}\`, ref \`${REF}\`)"
        echo "- OS: ${os_name}"
        echo "- Kernel: ${kernel}$([[ "${kernel,,}" == *microsoft* ]] && echo " (WSL)")"
        echo "- Date: $(date -u +'%Y-%m-%d %H:%M UTC')"
        echo "- Display: ${DISPLAY_MODE:-not checked}; build jobs: ${JOBS}; perf duration: ${PERF_DURATION}s; bench: $([[ "${FULL_BENCH}" -eq 1 ]] && echo "full" || echo "shortened (1 repetition, 0.05 s min time)")"
        echo
        echo "| # | Step | Result | Duration | Notes |"
        echo "|---|------|--------|----------|-------|"
        local i
        for i in "${!STEP_IDS[@]}"; do
            printf '| %d | %s | %s | %s | %s |\n' $((i + 1)) "${STEP_IDS[$i]}" "${STEP_RESULTS[$i]}" \
                "${STEP_DURATIONS[$i]}" "$(md_cell "${STEP_NOTES[$i]}")"
        done
        echo
        echo "Logs: \`${LOG_DIR}\`"
    } > "${RESULTS_FILE}"
}

RESULTS_WRITTEN=0
cleanup() {
    # An interrupted run still leaves the table of the steps it finished.
    if [[ "${RESULTS_WRITTEN}" -eq 0 && ${#STEP_IDS[@]} -gt 0 ]]; then
        write_results 2>/dev/null || true
        echo "Partial results: ${RESULTS_FILE}" >&2
    fi
    if [[ -n "${SUDO_KEEPALIVE_PID}" ]]; then
        kill "${SUDO_KEEPALIVE_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'echo "Interrupted." >&2; exit 130' INT TERM

# ---------- steps --------------------------------------------------------------------------------

step_preflight() {
    local os_id="" os_version="" pretty=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        os_id="$(. /etc/os-release && echo "${ID:-}")"
        # shellcheck disable=SC1091
        os_version="$(. /etc/os-release && echo "${VERSION_ID:-}")"
        # shellcheck disable=SC1091
        pretty="$(. /etc/os-release && echo "${PRETTY_NAME:-}")"
    fi
    echo "OS:      ${pretty:-unknown}"
    echo "Kernel:  $(uname -r)"
    echo "CPUs:    $(nproc)   Memory: $(awk '/^MemTotal/ { printf "%.1f GiB", $2 / 1048576 }' /proc/meminfo)"
    echo "Workdir: ${WORKDIR}"
    echo "Jobs:    ${JOBS} (CMAKE_BUILD_PARALLEL_LEVEL)"
    if [[ "${os_id}" != "ubuntu" || "${os_version}" != "24.04" ]]; then
        note "not Ubuntu 24.04 (${pretty:-unknown}); the plan targets a fresh Ubuntu 24.04 distro"
    fi
    if [[ "$(uname -r)" != *[Mm]icrosoft* ]]; then
        note "not a WSL kernel"
    fi

    if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
        DISPLAY_MODE="desktop (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset}, DISPLAY=${DISPLAY:-unset})"
        GUI_WRAP=()
    else
        DISPLAY_MODE="headless (xvfb-run -a)"
        GUI_WRAP=(xvfb-run -a)
        note "no WAYLAND_DISPLAY or DISPLAY: the app runs under xvfb-run"
    fi
    echo "Display: ${DISPLAY_MODE}"

    # The docs' commands use sudo (apt, setup-dev.sh). Ask once now and keep the ticket fresh, so a
    # password prompt never stops the run halfway through a step.
    command -v sudo >/dev/null 2>&1 || { fail "sudo not found; the docs' setup needs it"; return 1; }
    if ! sudo -n true 2>/dev/null; then
        echo "sudo needs your password (the docs' apt and setup-dev.sh steps use sudo):"
        sudo -v || { fail "sudo -v failed"; return 1; }
    fi
    (
        while sleep 50; do
            kill -0 "$$" 2>/dev/null || exit 0
            sudo -n true 2>/dev/null || exit 0
        done
    ) >/dev/null 2>&1 &
    SUDO_KEEPALIVE_PID=$!
    return 0
}

step_git() {
    if command -v git >/dev/null 2>&1; then
        echo "git is already installed: $(git --version)"
        note "git preinstalled ($(git --version | awk '{ print $3 }'))"
        return 0
    fi
    # Neither README.md nor CONTRIBUTING.md says to install git: Quick Start begins with git clone.
    finding "git was not installed and no doc says to install it (CONTRIBUTING Quick Start starts with git clone); installed it with apt"
    run sudo apt-get update || { fail "apt-get update failed"; return 1; }
    run sudo apt-get install -y git </dev/null || { fail "apt-get install git failed"; return 1; }
    git --version
}

step_clone() {
    # CONTRIBUTING.md "Quick Start": git clone https://github.com/mgradwohl/tasksmack.git; cd tasksmack
    run git clone "${REPO_URL}" "${CLONE_DIR}" || { fail "git clone ${REPO_URL} failed"; return 1; }
    cite CONTRIBUTING.md "git clone https://github.com/mgradwohl/tasksmack.git"
    cite CONTRIBUTING.md "cd tasksmack"
    cd "${CLONE_DIR}" || { fail "cd ${CLONE_DIR} failed"; return 1; }
    if ! run git checkout --detach "${REF}" 2>/dev/null &&
        ! run git checkout --detach "origin/${REF}" 2>/dev/null; then
        run git fetch origin "${REF}" || { fail "could not find or fetch ref ${REF}"; return 1; }
        run git checkout --detach FETCH_HEAD || { fail "could not check out ${REF}"; return 1; }
    fi
    COMMIT="$(git rev-parse HEAD)"
    echo "Testing commit ${COMMIT}: $(git log -1 --format='%s (%cd)' --date=short)"
    note "commit $(git rev-parse --short=10 HEAD)"
}

step_readme() {
    cd "${CLONE_DIR}" || return 1
    # README.md is the landing page; its Building from Source section links CONTRIBUTING's Quick Start.
    local ref
    ref="$(doc_line README.md "(CONTRIBUTING.md#quick-start)")"
    if [[ -n "${ref}" ]]; then
        echo "Doc: ${ref}: $(sed -n "${ref##*:}p" README.md)"
        return 0
    fi
    ref="$(doc_line README.md "(CONTRIBUTING.md)")"
    if [[ -z "${ref}" ]]; then
        fail "README.md has no link to CONTRIBUTING.md, so a newcomer cannot find the setup steps"
        return 1
    fi
    echo "Doc: ${ref}: $(sed -n "${ref##*:}p" README.md)"
    finding "README.md has no link to CONTRIBUTING's Quick Start; the only route to the build steps is its link to CONTRIBUTING.md (${ref})"
    return 0
}

# The Quick Start's Python commands, in its order. Returns 1 at the first one that fails.
quickstart_python() {
    cite CONTRIBUTING.md "python3.14 -m venv .venv"
    run python3.14 -m venv .venv || return 1
    cite CONTRIBUTING.md "source .venv/bin/activate"
    activate_venv || return 1
    cite CONTRIBUTING.md "python -m pip install -r requirements.txt"
    run python -m pip install -r requirements.txt || return 1
    cite CONTRIBUTING.md "pre-commit install"
    run pre-commit install || return 1
}

activate_venv() {
    echo "+ source .venv/bin/activate"
    [[ -f .venv/bin/activate ]] || { echo ".venv/bin/activate does not exist" >&2; return 1; }
    # The activate script reads variables that may be unset.
    set +u
    # shellcheck disable=SC1091
    source .venv/bin/activate
    local rc=$?
    set -u
    return "${rc}"
}

step_quickstart() {
    cd "${CLONE_DIR}" || return 1
    # Quick Start comes first in CONTRIBUTING.md and needs Python 3.14 before any setup step has
    # installed it; Ubuntu 24.04 ships 3.12.
    if ! command -v python3.14 >/dev/null 2>&1; then
        cite CONTRIBUTING.md "python3.14 -m venv .venv"
        finding "Quick Start runs python3.14 before any step installs it (Ubuntu 24.04 ships Python $(python3 -c 'import sys; print(f"{sys.version_info[0]}.{sys.version_info[1]}")' 2>/dev/null || echo '?')); its venv/pip/pre-commit commands cannot run until 'Automated Setup (Linux)' below. Retried after setup-dev.sh"
        QUICKSTART_DEFERRED=1
        return 0
    fi
    if ! quickstart_python; then
        fail "a Quick Start Python command failed before setup (see the log)"
        return 1
    fi
}

step_setup_dev() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "./tools/setup-dev.sh"
    # setup-dev.sh has no --yes/non-interactive flag; it runs apt-get install -y under sudo. stdin
    # is /dev/null so an unexpected prompt (debconf, add-apt-repository) fails instead of hanging.
    if ! run ./tools/setup-dev.sh </dev/null; then
        fail "tools/setup-dev.sh failed"
        return 1
    fi
    # The FlameGraph clone is best-effort inside setup-dev.sh: report it if it did not happen.
    [[ -d "${HOME}/opt/FlameGraph" ]] || note "setup-dev.sh did not leave ~/opt/FlameGraph (optional)"
    if ! command -v perf >/dev/null 2>&1; then
        note "perf not installed (setup-dev.sh leaves it as a manual step)"
    fi
}

step_activate() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "source .venv/bin/activate     # Use the project-local Python environment"
    activate_venv || { fail "could not activate .venv after setup-dev.sh"; return 1; }
    echo "python: $(command -v python) ($(python --version 2>&1))"
    if [[ "${QUICKSTART_DEFERRED}" -eq 1 ]]; then
        echo "Retrying the Quick Start Python commands now that setup-dev.sh installed Python 3.14:"
        if ! quickstart_python; then
            fail "a Quick Start Python command still fails after setup-dev.sh (see the log)"
            return 1
        fi
        note "Quick Start Python commands work after setup-dev.sh"
    fi
}

step_check_prereqs() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "./tools/check-prereqs.sh      # Verify environment after setup"
    if ! run ./tools/check-prereqs.sh </dev/null; then
        fail "tools/check-prereqs.sh reports missing mandatory prerequisites right after tools/setup-dev.sh"
        return 1
    fi
}

step_dev_workflow() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "cmake --workflow --preset dev"
    echo "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL}"
    note "builds with CMAKE_BUILD_PARALLEL_LEVEL=${JOBS}; CONTRIBUTING does not say how to cap build jobs"
    if ! run cmake --workflow --preset dev; then
        fail "cmake --workflow --preset dev failed"
        return 1
    fi
}

step_ctest() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "ctest --preset debug"
    if ! run ctest --preset debug; then
        fail "ctest --preset debug failed"
        return 1
    fi
}

# Wait for TEXT in FILE while PID lives, up to SECONDS. 0 found, 1 timed out, 2 the process exited.
wait_for_log() {
    local file="$1" text="$2" pid="$3" timeout="$4"
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        grep -qF -- "${text}" "${file}" 2>/dev/null && return 0
        kill -0 "${pid}" 2>/dev/null || { grep -qF -- "${text}" "${file}" 2>/dev/null && return 0; return 2; }
        sleep 0.25
    done
    return 1
}

step_smoke_launch() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "the binary is under \`build/debug/bin/TaskSmack\`"
    local binary="${CLONE_DIR}/build/debug/bin/TaskSmack"
    [[ -x "${binary}" ]] || { fail "${binary} was not built"; return 1; }
    if [[ ${#GUI_WRAP[@]} -gt 0 ]] && ! command -v xvfb-run >/dev/null 2>&1; then
        fail "headless and xvfb-run is missing (tools/setup-dev.sh should have installed xvfb)"
        return 1
    fi

    local config_dir app_log
    config_dir="$(mktemp -d "${WORKDIR}/smoke-config.XXXXXX")"
    app_log="${LOG_DIR}/smoke-launch-app.log"
    echo "TASKSMACK_CONFIG_DIR=${config_dir}"
    echo "App log: ${app_log}"

    # The markers tools/measure-idle.sh reads: "Entering main loop" (Core::Application::run), then
    # the ResizePerf[...] frame summaries TASKSMACK_TRACE_RESIZE_PERF=1 logs every 5 s at idle,
    # which show frames being presented.
    local main_loop="Entering main loop" frames="ResizePerf[" locked="TaskSmack is already running"
    printf '+ TASKSMACK_CONFIG_DIR=%q TASKSMACK_TRACE_RESIZE_PERF=1 TASKSMACK_LOG_LEVEL=info' "${config_dir}"
    printf ' %q' "${GUI_WRAP[@]}" "${binary}"
    printf '\n'
    env TASKSMACK_CONFIG_DIR="${config_dir}" TASKSMACK_TRACE_RESIZE_PERF=1 TASKSMACK_LOG_LEVEL=info \
        "${GUI_WRAP[@]}" "${binary}" > "${app_log}" 2>&1 </dev/null &
    local wait_pid=$! app_pid="" i
    # On a desktop env execs TaskSmack, so $! is TaskSmack. Under xvfb-run, TaskSmack is its child.
    if [[ ${#GUI_WRAP[@]} -eq 0 ]]; then
        app_pid="${wait_pid}"
    else
        for ((i = 0; i < 120; i++)); do
            app_pid="$(pgrep -x -P "${wait_pid}" TaskSmack 2>/dev/null | head -n 1)" || true
            [[ -n "${app_pid}" ]] && break
            kill -0 "${wait_pid}" 2>/dev/null || break
            sleep 0.25
        done
        if [[ -z "${app_pid}" ]]; then
            kill "${wait_pid}" 2>/dev/null || true
            wait "${wait_pid}" 2>/dev/null || true
            fail "TaskSmack did not start under xvfb-run (see ${app_log})"
            return 1
        fi
    fi
    echo "TaskSmack pid ${app_pid}"

    local rc=0
    wait_for_log "${app_log}" "${main_loop}" "${app_pid}" 30 || rc=$?
    if grep -qF -- "${locked}" "${app_log}" 2>/dev/null; then
        kill -KILL "${app_pid}" 2>/dev/null || true
        wait "${wait_pid}" 2>/dev/null || true
        fail "TaskSmack hit the single-instance lock despite a fresh TASKSMACK_CONFIG_DIR"
        return 1
    fi
    if [[ "${rc}" -ne 0 ]]; then
        kill -KILL "${app_pid}" 2>/dev/null || true
        local exit_code=0
        wait "${wait_pid}" 2>/dev/null || exit_code=$?
        tail -n 30 "${app_log}"
        if [[ "${rc}" -eq 2 ]]; then
            fail "TaskSmack exited (code ${exit_code}) before logging '${main_loop}'"
        else
            fail "TaskSmack did not log '${main_loop}' within 30 s"
        fi
        return 1
    fi
    echo "Main loop running."
    rc=0
    wait_for_log "${app_log}" "${frames}" "${app_pid}" 20 || rc=$?
    if [[ "${rc}" -ne 0 ]]; then
        kill -KILL "${app_pid}" 2>/dev/null || true
        wait "${wait_pid}" 2>/dev/null || true
        tail -n 30 "${app_log}"
        fail "no ResizePerf frame summary within 20 s of the main loop starting (no frames presented?)"
        return 1
    fi
    echo "First frames presented: $(grep -F -m1 -- "${frames}" "${app_log}" | cut -c1-200)"
    local renderer
    renderer="$(grep -F -m1 'Renderer:' "${app_log}" | sed 's/.*Renderer: *//')" || true
    [[ -n "${renderer}" ]] && note "renderer: ${renderer}"

    echo "+ kill -TERM ${app_pid}"
    kill -TERM "${app_pid}" 2>/dev/null || true
    local killed=0
    for ((i = 0; i < 40; i++)); do
        kill -0 "${app_pid}" 2>/dev/null || break
        sleep 0.25
    done
    if kill -0 "${app_pid}" 2>/dev/null; then
        kill -KILL "${app_pid}" 2>/dev/null || true
        killed=1
    fi
    local exit_code=0
    wait "${wait_pid}" 2>/dev/null || exit_code=$?
    rm -rf "${config_dir}"
    if [[ "${killed}" -eq 1 ]]; then
        fail "TaskSmack did not exit within 10 s of SIGTERM and was killed"
        return 1
    fi
    if [[ "${exit_code}" -ne 0 ]]; then
        tail -n 30 "${app_log}"
        fail "TaskSmack exited with code ${exit_code} after SIGTERM"
        return 1
    fi
    echo "TaskSmack exited 0 after SIGTERM."
    note "window up, frames presented, clean exit 0 on SIGTERM (${DISPLAY_MODE%% *})"
}

step_benchmark_build() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "cmake --preset benchmark"
    run cmake --preset benchmark || { fail "cmake --preset benchmark failed"; return 1; }
    cite CONTRIBUTING.md "cmake --build --preset benchmark"
    run cmake --build --preset benchmark || { fail "cmake --build --preset benchmark failed"; return 1; }
    local bin="build/benchmark/bin/TaskSmackBenchmarks"
    cite CONTRIBUTING.md "./build/benchmark/bin/TaskSmackBenchmarks"
    [[ -x "${bin}" ]] || { fail "${bin} was not built"; return 1; }
    # The doc runs the binary directly next; listing its benchmarks proves it starts without
    # spending the full run twice (bench.sh runs them all in the next step).
    echo "+ ./${bin} --benchmark_list_tests=true > ${LOG_DIR}/benchmark-list.txt"
    "./${bin}" --benchmark_list_tests=true > "${LOG_DIR}/benchmark-list.txt" ||
        { fail "${bin} --benchmark_list_tests failed"; return 1; }
    note "$(wc -l < "${LOG_DIR}/benchmark-list.txt") benchmarks listed"
}

step_bench() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "./tools/bench.sh benchmark"
    local args=(./tools/bench.sh benchmark)
    if [[ "${FULL_BENCH}" -eq 0 ]]; then
        # Google Benchmark takes the last value of a flag, so these override bench.sh's own.
        args+=(-- --benchmark_repetitions=1 --benchmark_min_time=0.05s)
        note "shortened: --benchmark_repetitions=1 --benchmark_min_time=0.05s (--full-bench for the documented run)"
    fi
    run "${args[@]}" </dev/null || { fail "tools/bench.sh failed"; return 1; }
}

step_measure_idle() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "./tools/measure-idle.sh --label overview"
    if [[ ${#GUI_WRAP[@]} -gt 0 ]] && ! command -v xvfb-run >/dev/null 2>&1; then
        fail "headless and xvfb-run is missing"
        return 1
    fi
    # The documented default builds the profile preset first, then measures it.
    run "${GUI_WRAP[@]}" ./tools/measure-idle.sh --label overview --duration "${PERF_DURATION}" </dev/null ||
        { fail "tools/measure-idle.sh failed"; return 1; }
    note "--duration ${PERF_DURATION} (15 s warm-up); WSL figures are CPU-only evidence"
}

step_profile_perf() {
    cd "${CLONE_DIR}" || return 1
    cite CONTRIBUTING.md "./tools/profile-perf.sh app --warmup 10 --duration 30"
    if ! command -v perf >/dev/null 2>&1 || ! perf --version >/dev/null 2>&1; then
        echo "perf is not usable: $(perf --version 2>&1 | head -n 2 || true)"
        skip "perf not installed or not usable; CONTRIBUTING and setup-dev.sh leave installing it as a manual step (check-prereqs.sh prints the command)"
        return 0
    fi
    if [[ ${#GUI_WRAP[@]} -gt 0 ]] && ! command -v xvfb-run >/dev/null 2>&1; then
        fail "headless and xvfb-run is missing"
        return 1
    fi
    run "${GUI_WRAP[@]}" ./tools/profile-perf.sh app --warmup 10 --duration "${PERF_DURATION}" </dev/null ||
        { fail "tools/profile-perf.sh app failed"; return 1; }
    note "--warmup 10 --duration ${PERF_DURATION}"
}

# ---------- main ---------------------------------------------------------------------------------

echo "TaskSmack newcomer check (Linux) — repo ${REPO_URL} ref ${REF}, workdir ${WORKDIR}"

run_step preflight       step_preflight
run_step git             step_git
run_step clone           step_clone
run_step readme          step_readme
run_step quickstart      step_quickstart
run_step setup-dev       step_setup_dev
run_step activate        step_activate
run_step check-prereqs   step_check_prereqs
run_step dev-workflow    step_dev_workflow
run_step ctest           step_ctest
run_step smoke-launch    step_smoke_launch
run_step benchmark-build step_benchmark_build
run_step bench           step_bench
run_step measure-idle    step_measure_idle
run_step profile-perf    step_profile_perf

write_results
RESULTS_WRITTEN=1
echo
echo "==================================================================================="
cat "${RESULTS_FILE}"
echo "==================================================================================="
echo "Results table: ${RESULTS_FILE}"

[[ "${ANY_FAIL}" -eq 0 ]]
