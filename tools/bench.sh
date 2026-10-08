#!/usr/bin/env bash
# bench.sh — Run TaskSmack benchmarks with consistent settings.
#
# Usage:
#   ./tools/bench.sh [preset] [-- <extra args>]
#
# preset defaults to 'benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json (-2, -3, ... appended when a run in
# the same second already wrote that name), holding every repetition plus the
# mean/median/stddev/cv aggregates, and a provenance sidecar at
# perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config, benchmark
# args, anonymized machine class; written by tools/bench-manifest.py, see CONTRIBUTING.md
# "Benchmark Output").
#
# Environment overrides (used by the script tests):
#   TASKSMACK_BENCH_BIN      benchmark binary (default build/<preset>/bin/TaskSmackBenchmarks); a
#                            relative path or bare name is relative to the current directory and
#                            is never looked up on PATH
#   TASKSMACK_BENCH_OUT_DIR  output directory (default perf-data/)
#
# Exits non-zero if the benchmark binary fails or crashes; any partial output is still redacted
# (or deleted when it cannot be) and the manifest records the exit code.
#
# Examples:
#   ./tools/bench.sh                          # benchmark preset, default flags
#   ./tools/bench.sh debug                    # debug preset (slower, useful for profiling)
#   ./tools/bench.sh -- --benchmark_filter=ProcessModel
#   ./tools/bench.sh benchmark -- --benchmark_filter=ProcessModel

set -euo pipefail
# An inherited CDPATH would make the `cd`s below search it, and print where they went, inside the
# command substitutions that resolve paths (#1445 review).
unset CDPATH

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

# ---------- defaults ---------------------------------------------------------
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

mkdir -p "${OUT_DIR}"

# Claim the result name before the benchmark starts, atomically (noclobber opens with O_EXCL, so
# the redirect fails if the file exists), so two runs in the same second -- concurrent ones too --
# never share a name: the later one gets -2, -3, ... A name whose manifest is left from an earlier
# run is skipped as well. The manifest name follows the claimed result name. Kept in step with the
# claim in bench.ps1.
claim_output() {
    (set -o noclobber && : >"$1") 2>/dev/null
}
STEM="${PRESET}-${TIMESTAMP}"
SUFFIX=2
until [[ ! -e "${OUT_DIR}/${STEM}.manifest.json" ]] && claim_output "${OUT_DIR}/${STEM}.json"; do
    if [[ ! -e "${OUT_DIR}/${STEM}.json" && ! -e "${OUT_DIR}/${STEM}.manifest.json" ]]; then
        echo "Cannot create '${OUT_DIR}/${STEM}.json'." >&2
        exit 1
    fi
    STEM="${PRESET}-${TIMESTAMP}-${SUFFIX}"
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
echo

# Provenance is captured before the benchmark starts (#1445 review): git state, build
# configuration and the binary's hash describe what was launched, even if the checkout, the build
# or the binary changes during the run. Only the exit code is added afterwards. The manifest is
# finalized before the result is redacted, so the result file stays the newest one in the output
# directory (heavy-checks.yml picks the latest perf-data/benchmark-*.json). python3 is already a
# build prerequisite (GLAD loader generation). A manifest failure is held until the result has
# been redacted, then fails the script.
MANIFEST_EXIT=0
python3 "${SCRIPT_DIR}/bench-manifest.py" \
    --manifest "${MANIFEST_FILE}" \
    --result "${OUT_FILE}" \
    --binary "${BENCH_BIN}" \
    --preset "${PRESET}" \
    --repo-root "${REPO_ROOT}" \
    -- "${BENCH_ARGS[@]}" || MANIFEST_EXIT=$?

# Capture the exit code instead of letting `set -e` abort here, so a failed run's partial output
# is still redacted (or deleted) before the script fails (#1423).
BENCH_EXIT=0
"${BENCH_BIN}" "${BENCH_ARGS[@]}" || BENCH_EXIT=$?

if [[ ${MANIFEST_EXIT} -eq 0 ]]; then
    python3 "${SCRIPT_DIR}/bench-manifest.py" --manifest "${MANIFEST_FILE}" --finalize \
        --exit-code "${BENCH_EXIT}" || MANIFEST_EXIT=$?
fi

# Redact machine-identifying context so results are safe to commit (repo convention:
# host_name "redacted", bare executable name).
redact_result() {
    python3 - "${OUT_FILE}" <<'PY'
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

echo
echo "Results written to: ${OUT_FILE}"
echo "Provenance manifest: ${MANIFEST_FILE}"
echo "Compare two runs with Google Benchmark's compare.py:"
echo "  python -m google_benchmark.compare perf-data/linux-baseline.json ${OUT_FILE}"
