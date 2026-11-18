#!/usr/bin/env bash
# bench.sh — Run TaskSmack benchmarks with consistent settings.
#
# Usage:
#   ./tools/bench.sh [--core performance|any] [--baseline FILE [--launches N] [--budget-seconds S]
#                    [--check-arg ARG]...] [preset] [-- <extra args>]
#   ./tools/bench.sh -h | --help              # print this help
#
# preset defaults to 'benchmark'.
# --baseline FILE (#3912, as bench.ps1's -Baseline, #3448): once the run succeeds, check it against FILE
# with tools/check-benchmark-regression.py (its default thresholds, then each --check-arg, e.g.
# --check-arg=--threshold=40; --baseline, --current, --current-manifest and --regressions-out, which
# the script sets itself, are refused there, and so is any abbreviation of them). When the check flags regressions, the flagged benchmarks
# alone run again in N more separate launches (--launches, default 4: 5 in all with the full run),
# each a bench.sh run of its own with the same preset, binary, --core and arguments plus the plan's
# --benchmark_filter, and each one's median across the launches' medians is checked again at the same
# thresholds and coverage: heavy-checks.yml's method, through the same tools/bench-across-launches.py
# run. Not best-of-N, so a benchmark slow in most launches still fails. The launches may take
# --budget-seconds (default 600): past it, or when nothing can be re-measured (a coverage loss alone),
# the flags stand. A launch whose manifest records another core class than the full run's is exit 2,
# with nothing combined. The launches, the flagged names and the combined run go to
# <result>.across-launches/ next to the result. The script then exits with the last check's code: 0
# passed, 1 regressions or low coverage (or the flags stand), 2 unusable input, 3 a launch failed.
# --core (#3198, as bench.ps1's -Core): "performance" pins the run to the logical CPUs of the fastest
# class with taskset, leaving out CPU 0 and its SMT sibling (CPU 0 takes most interrupts) while that
# still leaves at least four, and never fewer than four (#3413: the concurrent benchmarks run four
# threads that spin on each other; with fewer of the fastest class the run isn't pinned, with a
# warning); "any" leaves placement to the scheduler. The default is "performance" on a hybrid CPU
# (Intel P and E cores, Arm big.LITTLE: more than one cpu_capacity or cpuinfo_max_freq in sysfs) and
# "any" elsewhere. Without the sysfs files or taskset the run isn't pinned, with a warning. The
# manifest records where the run went (placement) and the system CPU load just before and after it.
# Produces JSON output at perf-data/<preset>-<timestamp>.json (-2, -3, ... appended when a run in
# the same second already wrote that name; a user or host name in the preset becomes "user" /
# "host" in the file names), holding every repetition plus the
# mean/median/stddev/cv aggregates, and a provenance sidecar at
# perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config, benchmark
# args, anonymized machine class; written by tools/bench-manifest.py, see CONTRIBUTING.md
# "Benchmark Output"). On success the last line is RESULT=<result file path>, for scripts to read
# (a failed --baseline check still prints it, then exits with the check's code).
#
# Environment overrides (used by the script tests):
#   TASKSMACK_BENCH_BIN      benchmark binary (default build/<preset>/bin/TaskSmackBenchmarks); a
#                            relative path or bare name is relative to the current directory and
#                            is never looked up on PATH
#   TASKSMACK_BENCH_OUT_DIR  output directory (default perf-data/)
#   TASKSMACK_BENCH_SYSFS_CPU  the CPUs' sysfs directory (default /sys/devices/system/cpu)
#   TASKSMACK_BENCH_PROC_STAT  the file the load is read from (default /proc/stat)
#   TASKSMACK_BENCH_LOAD_SAMPLE_MS  how long each load sample lasts (default 500)
#
# Exits with the benchmark binary's own exit code if it fails or crashes; any partial output is
# still redacted (or deleted when it cannot be) and the manifest records the exit code. An extra
# --benchmark_out or --benchmark_out_format argument is refused with exit code 2. Needs Python
# 3.14+ (the project .venv, or python3 on PATH: find_python in tools/common.sh).
#
# Examples:
#   ./tools/bench.sh                          # benchmark preset, default flags
#   ./tools/bench.sh debug                    # debug preset (slower, useful for profiling)
#   ./tools/bench.sh -- --benchmark_filter=ProcessModel
#   ./tools/bench.sh --core any               # not pinned, even on a hybrid CPU
#   ./tools/bench.sh benchmark -- --benchmark_filter=ProcessModel
#
#   ./tools/bench.sh --baseline <a run of main>.json   # and check it, across launches
#
# There is no committed local Linux baseline (#4199): check a branch against a run of main on the same
# machine (CONTRIBUTING.md, "Running Benchmarks"). Compare two runs yourself (one launch's medians, which a
# noisy launch can fail):
#   python3 tools/check-benchmark-regression.py --baseline <a run of main>.json \
#       --current perf-data/benchmark-<timestamp>.json

set -euo pipefail
# An inherited CDPATH would make the `cd`s below search it, and print where they went, inside the
# command substitutions that resolve paths (#1445 review).
unset CDPATH

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

# -h / --help as the first argument prints the header above, as Get-Help does for bench.ps1
# (#2254). After a preset or "--" it is a benchmark argument, as before.
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,/^$/{/^$/d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"
    exit 0
fi

# find_python (#2254).
# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

# ---------- defaults ---------------------------------------------------------
# The script's options, before the preset: --core MODE (#3198) and --baseline FILE with its
# --launches, --budget-seconds and --check-arg (#3912), each as "--name value" or "--name=value".
CORE=""
BASELINE=""
LAUNCHES=4
BUDGET_SECONDS=600
CHECK_ARGS=()
while [[ $# -gt 0 ]]; do
    case "${1}" in
        --core|--baseline|--launches|--budget-seconds|--check-arg)
            NAME="${1}"; VALUE="${2-}"
            if [[ $# -lt 2 ]]; then
                echo "${NAME} needs a value." >&2
                exit 2
            fi
            shift 2 ;;
        --core=*|--baseline=*|--launches=*|--budget-seconds=*|--check-arg=*)
            NAME="${1%%=*}"; VALUE="${1#*=}"; shift ;;
        *) break ;;
    esac
    case "${NAME}" in
        --core)
            CORE="${VALUE}"
            if [[ "${CORE}" != "performance" && "${CORE}" != "any" ]]; then
                echo "--core takes 'performance' or 'any', not '${CORE}'." >&2
                exit 2
            fi ;;
        --baseline)
            BASELINE="${VALUE}"
            # Checked before the run, so a mistake fails now and not after a 25-minute run.
            if [[ ! -f "${BASELINE}" ]]; then
                echo "Baseline not found: ${BASELINE}" >&2
                exit 2
            fi
            BASELINE="$(cd -- "$(dirname -- "${BASELINE}")" && pwd)/$(basename -- "${BASELINE}")" ;;
        --launches)
            LAUNCHES="${VALUE}"
            if [[ ! "${LAUNCHES}" =~ ^[1-9][0-9]*$ ]]; then
                echo "--launches takes a whole number of at least 1, not '${LAUNCHES}'." >&2
                exit 2
            fi ;;
        --budget-seconds)
            BUDGET_SECONDS="${VALUE}"
            if [[ ! "${BUDGET_SECONDS}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
                echo "--budget-seconds takes a number of seconds, not '${BUDGET_SECONDS}'." >&2
                exit 2
            fi ;;
        --check-arg)
            # The checker options this script sets itself, or an abbreviation of one (the checker's
            # argparse accepts any unique prefix of a long option), are refused, as bench.ps1 does.
            CHECK_NAME="${VALUE%%=*}"
            if [[ "${CHECK_NAME}" == --?* ]]; then
                for OWNED in --baseline --current --current-manifest --regressions-out; do
                    if [[ "${OWNED}" == "${CHECK_NAME}"* ]]; then
                        echo "'${VALUE}' is not allowed in --check-arg: the checker reads it as ${OWNED}, which bench.sh sets for each check itself." >&2
                        exit 2
                    fi
                done
            fi
            CHECK_ARGS+=("${VALUE}") ;;
    esac
done
if [[ -z "${BASELINE}" ]] && { [[ "${LAUNCHES}" != 4 || "${BUDGET_SECONDS}" != 600 ]] || (( ${#CHECK_ARGS[@]} > 0 )); }; then
    echo "--launches, --budget-seconds and --check-arg need --baseline." >&2
    exit 2
fi
PRESET="${1:-benchmark}"
if [[ "${PRESET}" == "--" ]]; then
    PRESET="benchmark"
    shift
elif [[ "${PRESET}" == --* ]]; then
    # A benchmark flag with no preset before it (./tools/bench.sh --benchmark_filter=Foo): it stays
    # with the extra args and the default preset is used, as in bench.ps1.
    PRESET="benchmark"
elif [[ $# -gt 0 && "${1}" != "--" ]]; then
    shift
fi
# Drop optional "--" separator before extra args
if [[ $# -gt 0 && "${1}" == "--" ]]; then shift; fi

OUT_DIR="${TASKSMACK_BENCH_OUT_DIR:-${REPO_ROOT}/perf-data}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"

BENCH_BIN="${TASKSMACK_BENCH_BIN:-${REPO_ROOT}/build/${PRESET}/bin/TaskSmackBenchmarks}"

# ---------- sanity checks ----------------------------------------------------
if [[ ! -f "${BENCH_BIN}" ]]; then
    echo "Benchmark binary not found: ${BENCH_BIN}"
    echo "Build first:  cmake --build --preset ${PRESET}"
    exit 1
fi
# One absolute path for the check above, the manifest's hash and build lookup, and the launch: a
# relative TASKSMACK_BENCH_BIN (a bare name included) means the file relative to the current
# directory, as the check reads it -- never a same-named program found on PATH (#1445 review).
BENCH_BIN="$(cd -- "$(dirname -- "${BENCH_BIN}")" && pwd)/$(basename -- "${BENCH_BIN}")"

# The script owns the output file: Google Benchmark takes the last --benchmark_out(_format), so an
# extra one would write somewhere the redaction and the manifest never look.
for arg in "$@"; do
    if [[ "${arg}" =~ ^--benchmark_out(_format)?(=|$) ]]; then
        echo "'${arg}' is not allowed: bench.sh sets the benchmark output file and format itself." >&2
        echo "Set TASKSMACK_BENCH_OUT_DIR to choose where results are written." >&2
        exit 2
    fi
done

# The interpreter the build uses (#2254): the project .venv first, then python3.14 / python3 /
# python on PATH, each only when it is Python 3.14+ -- never a bare python3 that skips the .venv or,
# under Git Bash, is the Microsoft Store stub. Found before anything is written.
if ! PY="$(find_python)"; then
    echo "Python 3.14 or newer not found (the project .venv, or python3 on PATH); bench.sh needs it to write the manifest and redact the results." >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

# The Python helpers read and write UTF-8 whatever the locale (#1648); an explicit PYTHONUTF8 wins.
export PYTHONUTF8="${PYTHONUTF8:-1}"

# The preset part of the output file names (#1445 review): a user or host name in the preset
# becomes "user" / "host", so neither the files nor the manifest's result_file and --benchmark_out
# carry it (tools/bench-manifest.py hide_name_identity).
PRESET_STEM="$("${PY}" "${SCRIPT_DIR}/bench-manifest.py" --preset-stem "${PRESET}")"

# Claim the result name before the benchmark starts, atomically (noclobber opens with O_EXCL, so
# the redirect fails if the file exists), so two runs in the same second -- concurrent ones too --
# never share a name: the later one gets -2, -3, ... A name whose manifest is left from an earlier
# run is skipped as well. The manifest name follows the claimed result name. Kept in step with the
# claim in bench.ps1.
claim_output() {
    (set -o noclobber && : >"$1") 2>/dev/null
}
STEM="${PRESET_STEM}-${TIMESTAMP}"
SUFFIX=2
until [[ ! -e "${OUT_DIR}/${STEM}.manifest.json" ]] && claim_output "${OUT_DIR}/${STEM}.json"; do
    if [[ ! -e "${OUT_DIR}/${STEM}.json" && ! -e "${OUT_DIR}/${STEM}.manifest.json" ]]; then
        echo "Cannot create '${OUT_DIR}/${STEM}.json'." >&2
        exit 1
    fi
    STEM="${PRESET_STEM}-${TIMESTAMP}-${SUFFIX}"
    SUFFIX=$((SUFFIX + 1))
done
OUT_FILE="${OUT_DIR}/${STEM}.json"
MANIFEST_FILE="${OUT_DIR}/${STEM}.manifest.json"

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

# Core placement (#3198, #3413): the logical CPUs to pin the run to, as a taskset -c list, or none. Only the benchmark runs under taskset; the script's own work (git, hashing, the manifest)
# isn't confined to those CPUs. A failure to decide is a warning and an unpinned run with no
# placement recorded.
PLACEMENT_FILE="$(mktemp "${TMPDIR:-/tmp}/tasksmack-bench-placement.XXXXXX")"
trap 'rm -f "${PLACEMENT_FILE}"' EXIT
PIN_CPUS=""
if ! PIN_CPUS="$("${PY}" "${SCRIPT_DIR}/bench-manifest.py" --select-core ${CORE:+"${CORE}"} --placement-out "${PLACEMENT_FILE}")"; then
    echo "WARNING: choosing the cores failed, so the run is not pinned and no placement is recorded." >&2
    PIN_CPUS=""
    rm -f "${PLACEMENT_FILE}"
fi
PLACEMENT_ARGS=()
if [[ -s "${PLACEMENT_FILE}" ]]; then
    PLACEMENT_ARGS=(--placement "${PLACEMENT_FILE}")
fi
LAUNCH=("${BENCH_BIN}")
if [[ -n "${PIN_CPUS}" ]]; then
    LAUNCH=(taskset -c "${PIN_CPUS}" "${BENCH_BIN}")
fi
echo

# Provenance is captured before the benchmark starts (#1445 review): git state, build
# configuration and the binary's hash describe what was launched, even if the checkout, the build
# or the binary changes during the run. Only the exit code and the load after the run are added
# afterwards. The manifest is
# finalized before the result is redacted. Python is already a build prerequisite (GLAD loader
# generation). A manifest failure is held until the result has been redacted, then fails the
# script.
MANIFEST_EXIT=0
"${PY}" "${SCRIPT_DIR}/bench-manifest.py" \
    --manifest "${MANIFEST_FILE}" \
    --result "${OUT_FILE}" \
    --binary "${BENCH_BIN}" \
    --preset "${PRESET}" \
    --repo-root "${REPO_ROOT}" \
    ${PLACEMENT_ARGS[@]+"${PLACEMENT_ARGS[@]}"} \
    -- "${BENCH_ARGS[@]}" || MANIFEST_EXIT=$?

# Capture the exit code instead of letting `set -e` abort here, so a failed run's partial output
# is still redacted (or deleted) before the script fails (#1423).
BENCH_EXIT=0
"${LAUNCH[@]}" "${BENCH_ARGS[@]}" || BENCH_EXIT=$?

if [[ ${MANIFEST_EXIT} -eq 0 ]]; then
    "${PY}" "${SCRIPT_DIR}/bench-manifest.py" --manifest "${MANIFEST_FILE}" --finalize \
        --exit-code "${BENCH_EXIT}" || MANIFEST_EXIT=$?
fi

# Redact machine-identifying context so results are safe to commit (repo convention:
# host_name "redacted", bare executable name).
redact_result() {
    "${PY}" - "${OUT_FILE}" <<'PY'
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

# Output that cannot be redacted (empty or truncated JSON despite exit code 0) may still hold the
# host name, so it is deleted rather than left behind, and the script fails.
if ! redact_result; then
    rm -f "${OUT_FILE}"
    echo "Benchmark output '${OUT_FILE}' could not be redacted and was deleted." >&2
    exit 1
fi

if [[ ${MANIFEST_EXIT} -ne 0 ]]; then
    echo "Writing the provenance manifest '${MANIFEST_FILE}' failed (exit ${MANIFEST_EXIT})." >&2
    exit "${MANIFEST_EXIT}"
fi

# --baseline (#3912): the check, with any flagged benchmark measured across launches, as bench.ps1
# -Baseline and heavy-checks.yml do. Each further launch is this script again, in a process of its
# own, with the same preset, binary, --core and arguments plus the plan's --benchmark_filter (Google
# Benchmark takes the last one), so it is pinned, sampled, redacted and given a manifest the same way.
CHECK_EXIT=0
if [[ -n "${BASELINE}" ]]; then
    WORK_DIR="${OUT_DIR}/${STEM}.across-launches"
    mkdir -p "${WORK_DIR}"
    CHECKER="${SCRIPT_DIR}/check-benchmark-regression.py"
    echo
    echo "Checking against ${BASELINE}"
    "${PY}" "${CHECKER}" ${CHECK_ARGS[@]+"${CHECK_ARGS[@]}"} --baseline "${BASELINE}" --current "${OUT_FILE}" \
        --regressions-out "${WORK_DIR}/regressions.json" || CHECK_EXIT=$?
    # Only flagged regressions are re-measured; a pass, or a usage error, stands.
    if (( CHECK_EXIT == 1 )); then
        echo
        RUN_EXIT=0
        TASKSMACK_BENCH_BIN="${BENCH_BIN}" "${PY}" "${SCRIPT_DIR}/bench-across-launches.py" run \
            --current "${OUT_FILE}" --regressions "${WORK_DIR}/regressions.json" --baseline "${BASELINE}" \
            --output "${WORK_DIR}/combined.json" --work-dir "${WORK_DIR}" --first-manifest "${MANIFEST_FILE}" \
            --launches "${LAUNCHES}" --budget-seconds "${BUDGET_SECONDS}" \
            -- "${BASH}" "${SCRIPT_DIR}/bench.sh" ${CORE:+--core "${CORE}"} "${PRESET}" -- "$@" || RUN_EXIT=$?
        if (( RUN_EXIT == 0 )); then
            echo
            echo "Checking the median across $(( LAUNCHES + 1 )) launches against ${BASELINE}"
            CHECK_EXIT=0
            "${PY}" "${CHECKER}" ${CHECK_ARGS[@]+"${CHECK_ARGS[@]}"} --baseline "${BASELINE}" \
                --current "${WORK_DIR}/combined.json" --current-manifest "${MANIFEST_FILE}" || CHECK_EXIT=$?
        else
            CHECK_EXIT=${RUN_EXIT}
        fi
    fi
fi

echo
echo "Results written to: ${OUT_FILE}"
echo "Provenance manifest: ${MANIFEST_FILE}"
if [[ -n "${BASELINE}" ]]; then
    if (( CHECK_EXIT == 0 )); then
        echo "Checked against ${BASELINE}: passed"
    else
        echo "Checked against ${BASELINE}: failed (exit ${CHECK_EXIT})"
    fi
else
    echo "Compare against a run of main across launches with --baseline <that run's .json>, or on this run alone:"
    echo "  python3 tools/check-benchmark-regression.py --baseline <a run of main>.json --current ${OUT_FILE}"
fi
# Machine-readable result path, printed once the run succeeded (as the profile-* scripts print
# TRACE=), even when a --baseline check then fails: heavy-checks.yml reads it instead of guessing from
# a glob that also matches the manifest (#2249).
echo "RESULT=${OUT_FILE}"
exit "${CHECK_EXIT}"
