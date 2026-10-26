#!/usr/bin/env bash
# tools/analyze-perf.sh — Analyze a Linux perf CPU trace for TaskSmack.
#
# Usage:
#   ./tools/analyze-perf.sh <perf.data> [--top <n>] [--skip-flamegraph]
#                                        [--flamegraph-pl <path>]
#                                        [--stackcollapse-pl <path>]
#                                        [--allow-unresolved]
#
# A report whose TaskSmack functions are all bare addresses is an analysis failure (exit 3), not a summary:
# typically a perf built without libelf/DWARF (WSL's), which can't read the binaries' symbols (#2977).
# --allow-unresolved keeps such an address-only report (with a warning) for whoever wants it anyway.
#
# Outputs:
#   <basename>-report.txt           — perf report --stdio top-N functions
#   <basename>-flamegraph.svg       — interactive flamegraph (if flamegraph tools available)
#   <basename>-summary.txt          — combined summary with paths to all artifacts
#
# Requirements:
#   perf                            — always required
#   flamegraph.pl + stackcollapse-perf.pl  — optional; install via:
#                                     git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph
#                                     then add to PATH, or pass --flamegraph-pl / --stackcollapse-pl
#   hotspot                         — optional GUI: sudo apt install hotspot
#
# Examples:
#   ./tools/analyze-perf.sh perf-data/perf-app-20260604-120000.data
#   ./tools/analyze-perf.sh perf-data/perf-app-20260604-120000.data --top 30
#   ./tools/analyze-perf.sh perf-data/perf-bench-20260604-120000.data --skip-flamegraph

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

# ── argument parsing ──────────────────────────────────────────────────────────
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
fi
DATA_FILE="${1:-}"
shift || true

if [[ -z "${DATA_FILE}" ]]; then
    echo "Usage: $0 <perf.data> [--top <n>] [--skip-flamegraph] [--flamegraph-pl <path>] [--stackcollapse-pl <path>] [--allow-unresolved]" >&2
    exit 1
fi

TOP=20
SKIP_FLAMEGRAPH=0
FLAMEGRAPH_PL=""
STACKCOLLAPSE_PL=""
ALLOW_UNRESOLVED=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --top)              [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; TOP="$2";              shift 2 ;;
        --skip-flamegraph)  SKIP_FLAMEGRAPH=1;     shift   ;;
        --flamegraph-pl)    [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; FLAMEGRAPH_PL="$2";    shift 2 ;;
        --stackcollapse-pl) [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }; STACKCOLLAPSE_PL="$2"; shift 2 ;;
        --allow-unresolved) ALLOW_UNRESOLVED=1;    shift   ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if ! [[ "${TOP}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: --top must be a positive integer, got '${TOP}'" >&2
    exit 1
fi

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

print_step() {
    echo
    echo "──────────────────────────────────────────"
    echo "  $*"
    echo "──────────────────────────────────────────"
}

# Resolve flamegraph.pl: explicit arg > PATH search > common install locations
find_flamegraph_pl() {
    if [[ -n "${FLAMEGRAPH_PL}" ]]; then
        [[ -x "${FLAMEGRAPH_PL}" ]] || die "flamegraph.pl not found/executable at: ${FLAMEGRAPH_PL}"
        echo "${FLAMEGRAPH_PL}"
        return 0
    fi
    for candidate in \
        "$(command -v flamegraph.pl 2>/dev/null || true)" \
        "${HOME}/opt/FlameGraph/flamegraph.pl" \
        "/opt/FlameGraph/flamegraph.pl" \
        "$(command -v flamegraph 2>/dev/null || true)"; do
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 1
}

# Resolve stackcollapse-perf.pl: explicit arg > PATH search > common locations
find_stackcollapse_pl() {
    if [[ -n "${STACKCOLLAPSE_PL}" ]]; then
        [[ -x "${STACKCOLLAPSE_PL}" ]] || die "stackcollapse-perf.pl not found/executable at: ${STACKCOLLAPSE_PL}"
        echo "${STACKCOLLAPSE_PL}"
        return 0
    fi
    for candidate in \
        "$(command -v stackcollapse-perf.pl 2>/dev/null || true)" \
        "${HOME}/opt/FlameGraph/stackcollapse-perf.pl" \
        "/opt/FlameGraph/stackcollapse-perf.pl"; do
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 1
}

# ── pre-flight ────────────────────────────────────────────────────────────────
check_command perf "apt install linux-tools-generic" || die "perf is required."

[[ -f "${DATA_FILE}" ]] || die "perf data file not found: ${DATA_FILE}"

DATA_FILE="$(realpath "${DATA_FILE}")"
BASE_DIR="$(dirname "${DATA_FILE}")"
BASE_NAME="$(basename "${DATA_FILE}" .data)"
REPORT_FILE="${BASE_DIR}/${BASE_NAME}-report.txt"
FLAMEGRAPH_SVG="${BASE_DIR}/${BASE_NAME}-flamegraph.svg"
SUMMARY_FILE="${BASE_DIR}/${BASE_NAME}-summary.txt"

mkdir -p "${BASE_DIR}"

# ── perf report ───────────────────────────────────────────────────────────────
print_step "Generating perf report (top ${TOP})"

# --no-children: show self-CPU (exclusive), not inclusive/call-chain weight
# --stdio:       machine-readable text output
# -g:            include call graphs
# Write the whole report first, then trim it: piping perf straight into head would kill perf with
# SIGPIPE once head has its lines, and pipefail + set -e would then stop this script before the
# flamegraph and summary steps.
FULL_REPORT="$(mktemp)"
trap 'rm -f "${FULL_REPORT}"' EXIT
perf report \
    -i "${DATA_FILE}" \
    --stdio \
    --no-children \
    -g \
    --sort comm,dso,symbol \
    > "${FULL_REPORT}"
head -n $(( TOP * 10 + 50 )) "${FULL_REPORT}" > "${REPORT_FILE}"

info "Report: ${REPORT_FILE}"

# ── symbol resolution ─────────────────────────────────────────────────────────
# A row: "  8.94%  comm  dso  [.]  symbol". User-space rows ([.]) whose symbol is a bare address weren't
# resolved; TaskSmack's own binaries (its executables and test/benchmark programs) are the ones that matter,
# so a system library's unresolved rows only warn (#2977).
read -r APP_ROWS APP_UNRESOLVED SYSTEM_UNRESOLVED < <(awk '
    $1 ~ /^[0-9]+\.[0-9]+%$/ && $4 == "[.]" {
        app = ($3 ~ /^TaskSmack/)
        unresolved = ($5 ~ /^0x[0-9a-fA-F]+$/)
        if (app) { rows++; if (unresolved) appUnresolved++ }
        else if (unresolved) systemUnresolved++
    }
    END { printf "%d %d %d\n", rows, appUnresolved, systemUnresolved }' "${FULL_REPORT}")
explain_missing_symbols() {
    echo "  perf couldn't name TaskSmack's functions: they show as bare addresses." >&2
    echo "  This perf's symbol support (perf version --build-options):" >&2
    perf version --build-options 2>/dev/null | grep -E 'libelf|dwarf|libunwind' | sed 's/^/    /' >&2 || true
    echo "  A perf built without libelf/DWARF (WSL's, for one) can't read symbols: use a distribution perf that has" >&2
    echo "  them, or symbolize the addresses with llvm-symbolizer against the binary." >&2
}
if (( APP_ROWS > 0 && APP_UNRESOLVED == APP_ROWS )); then
    if [[ "${ALLOW_UNRESOLVED}" -eq 0 ]]; then
        echo "" >&2
        echo "ERROR: all ${APP_ROWS} of TaskSmack's sampled functions are unresolved addresses; not an analysis." >&2
        explain_missing_symbols
        echo "  Re-run with --allow-unresolved to keep the address-only report anyway: ${REPORT_FILE}" >&2
        exit 3
    fi
    echo "WARNING: all ${APP_ROWS} of TaskSmack's sampled functions are unresolved addresses (--allow-unresolved)." >&2
    explain_missing_symbols
elif (( APP_UNRESOLVED > 0 )); then
    echo "WARNING: ${APP_UNRESOLVED} of TaskSmack's ${APP_ROWS} sampled functions are unresolved addresses." >&2
fi
if (( SYSTEM_UNRESOLVED > 0 )); then
    info "Note: ${SYSTEM_UNRESOLVED} system-library rows have no symbols (their debug symbols aren't installed)."
fi

# ── flamegraph ────────────────────────────────────────────────────────────────
FLAMEGRAPH_GENERATED=0

if [[ "${SKIP_FLAMEGRAPH}" -eq 0 ]]; then
    FLAMEGRAPH_BIN="$(find_flamegraph_pl 2>/dev/null || true)"
    STACKCOLLAPSE_BIN="$(find_stackcollapse_pl 2>/dev/null || true)"

    if [[ -n "${FLAMEGRAPH_BIN}" && -n "${STACKCOLLAPSE_BIN}" ]]; then
        print_step "Generating flamegraph SVG"
        info "flamegraph.pl:       ${FLAMEGRAPH_BIN}"
        info "stackcollapse-perf:  ${STACKCOLLAPSE_BIN}"

        perf script -i "${DATA_FILE}" \
            | "${STACKCOLLAPSE_BIN}" \
            | "${FLAMEGRAPH_BIN}" \
            > "${FLAMEGRAPH_SVG}"

        FLAMEGRAPH_GENERATED=1
        info "SVG: ${FLAMEGRAPH_SVG}"
    else
        echo ""
        echo "NOTE: flamegraph tools not found — skipping SVG generation."
        echo "  Install: git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph"
        echo "  Then add ~/opt/FlameGraph to your PATH, or re-run with:"
        echo "    --flamegraph-pl ~/opt/FlameGraph/flamegraph.pl \\"
        echo "    --stackcollapse-pl ~/opt/FlameGraph/stackcollapse-perf.pl"
    fi
fi

# ── summary ───────────────────────────────────────────────────────────────────
{
    echo "Trace:  ${DATA_FILE}"
    echo "Report: ${REPORT_FILE}"
    if [[ "${FLAMEGRAPH_GENERATED}" -eq 1 ]]; then
        echo "SVG:    ${FLAMEGRAPH_SVG}"
    fi
    echo ""
    echo "Top ${TOP} functions (self CPU):"
    # grep's status, not head's: the fallback ran only when the pipeline failed, which head never did.
    TOP_ROWS="$(grep -E "^[[:space:]]+[0-9]+\.[0-9]+%" "${REPORT_FILE}" | grep -v "\[k\]" | head -n "${TOP}" || true)"
    if [[ -n "${TOP_ROWS}" ]]; then
        echo "${TOP_ROWS}"
    else
        echo "(No user-space functions sampled)"
    fi
    if (( APP_UNRESOLVED > 0 )); then
        echo ""
        echo "Unresolved: ${APP_UNRESOLVED} of TaskSmack's ${APP_ROWS} sampled functions are bare addresses."
    fi
} > "${SUMMARY_FILE}"

cat "${SUMMARY_FILE}"

# ── next steps ────────────────────────────────────────────────────────────────
echo ""
echo "PERF_DATA=${DATA_FILE}"
echo "REPORT=${REPORT_FILE}"
if [[ "${FLAMEGRAPH_GENERATED}" -eq 1 ]]; then
    echo "FLAMEGRAPH=${FLAMEGRAPH_SVG}"
fi
echo "SUMMARY=${SUMMARY_FILE}"
echo ""
echo "Next steps:"
if command -v hotspot &>/dev/null; then
    echo "  hotspot ${DATA_FILE}       # interactive GUI"
fi
if [[ "${FLAMEGRAPH_GENERATED}" -eq 1 ]]; then
    echo "  xdg-open ${FLAMEGRAPH_SVG} # open flamegraph in browser"
fi
echo "  perf report -i ${DATA_FILE}  # interactive TUI"
