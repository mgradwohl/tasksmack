#!/usr/bin/env bash
# tools/analyze-perf.sh — Analyze a Linux perf CPU trace for TaskSmack.
#
# Usage:
#   ./tools/analyze-perf.sh <perf.data> [--top <n>] [--skip-flamegraph]
#                                        [--flamegraph-pl <path>]
#                                        [--stackcollapse-pl <path>]
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
DATA_FILE="${1:-}"
shift || true

if [[ -z "${DATA_FILE}" ]]; then
    echo "Usage: $0 <perf.data> [--top <n>] [--skip-flamegraph] [--flamegraph-pl <path>] [--stackcollapse-pl <path>]" >&2
    exit 1
fi

TOP=20
SKIP_FLAMEGRAPH=0
FLAMEGRAPH_PL=""
STACKCOLLAPSE_PL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --top)              TOP="$2";              shift 2 ;;
        --skip-flamegraph)  SKIP_FLAMEGRAPH=1;     shift   ;;
        --flamegraph-pl)    FLAMEGRAPH_PL="$2";    shift 2 ;;
        --stackcollapse-pl) STACKCOLLAPSE_PL="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

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
        "/usr/share/perl5/Flamegraph.pm" \
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
perf report \
    -i "${DATA_FILE}" \
    --stdio \
    --no-children \
    -g \
    --sort comm,dso,symbol \
    2>/dev/null \
    | head -n $(( TOP * 10 + 50 )) \
    > "${REPORT_FILE}"

info "Report: ${REPORT_FILE}"

# ── flamegraph ────────────────────────────────────────────────────────────────
FLAMEGRAPH_GENERATED=0

if [[ "${SKIP_FLAMEGRAPH}" -eq 0 ]]; then
    FLAMEGRAPH_BIN="$(find_flamegraph_pl 2>/dev/null || true)"
    STACKCOLLAPSE_BIN="$(find_stackcollapse_pl 2>/dev/null || true)"

    if [[ -n "${FLAMEGRAPH_BIN}" && -n "${STACKCOLLAPSE_BIN}" ]]; then
        print_step "Generating flamegraph SVG"
        info "flamegraph.pl:       ${FLAMEGRAPH_BIN}"
        info "stackcollapse-perf:  ${STACKCOLLAPSE_BIN}"

        perf script -i "${DATA_FILE}" 2>/dev/null \
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

# ── print top functions ───────────────────────────────────────────────────────
print_step "Top ${TOP} functions"

# Extract the symbol table section from the perf report
# perf report --stdio emits lines like:  "  4.12%  TaskSmack  TaskSmack  [.] functionName"
# Filter to TaskSmack binary entries and print the top N
{
    grep -E "^\s+[0-9]+\.[0-9]+%" "${REPORT_FILE}" \
        | grep -v "\[k\]" \
        | head -n "${TOP}" \
        || echo "(No symbols found — the binary may lack debug info or DWARF unwinding failed.)"
} | tee /dev/stderr > /tmp/tasksmack_top_functions_$$.txt 2>&1 || true

# ── summary ───────────────────────────────────────────────────────────────────
{
    echo "Trace:  ${DATA_FILE}"
    echo "Report: ${REPORT_FILE}"
    if [[ "${FLAMEGRAPH_GENERATED}" -eq 1 ]]; then
        echo "SVG:    ${FLAMEGRAPH_SVG}"
    fi
    echo ""
    echo "Top ${TOP} functions (self CPU):"
    grep -E "^\s+[0-9]+\.[0-9]+%" "${REPORT_FILE}" \
        | grep -v "\[k\]" \
        | head -n "${TOP}" \
        || echo "(No symbols found)"
} > "${SUMMARY_FILE}"

cat "${SUMMARY_FILE}"

rm -f /tmp/tasksmack_top_functions_$$.txt

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
