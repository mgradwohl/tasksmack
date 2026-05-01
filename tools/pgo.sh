#!/usr/bin/env bash
# tools/pgo.sh – End-to-end Profile-Guided Optimization workflow for Linux
#
# Usage:
#   ./tools/pgo.sh             # Full PGO workflow (generate + merge + use)
#   ./tools/pgo.sh generate    # Step 1 only: instrumented build + run to collect data
#   ./tools/pgo.sh merge       # Step 2 only: merge *.profraw → profiles/tasksmack.profdata
#   ./tools/pgo.sh use         # Step 3 only: build PGO-optimized binary
#
# The resulting binary is at: build/pgo-use/bin/TaskSmack
#
# Requirements:
#   - clang++ (project Clang version, see CONTRIBUTING.md)
#   - llvm-profdata (ships with LLVM)
#   - cmake, ninja
#
# Background: Clang PGO works in three phases:
#   1. Build with -fprofile-instr-generate (instrumented binary that records branch counts)
#   2. Run the instrumented binary; LLVM_PROFILE_FILE controls output filename
#   3. Merge the per-run .profraw files into a single .profdata with llvm-profdata
#   4. Build again with -fprofile-instr-use=<path>.profdata for an optimised binary

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFILES_DIR="${ROOT}/profiles"
PROFRAW_PATTERN="${PROFILES_DIR}/tasksmack-%p.profraw"
PROFDATA="${PROFILES_DIR}/tasksmack.profdata"
BENCH_BIN="${ROOT}/build/pgo-generate/bin/TaskSmackBenchmarks"
APP_BIN="${ROOT}/build/pgo-generate/bin/TaskSmack"

# ── helpers ──────────────────────────────────────────────────────────────────

print_step() { echo; echo "──────────────────────────────────────────"; echo "  $*"; echo "──────────────────────────────────────────"; }
die() { echo "ERROR: $*" >&2; exit 1; }

require_cmd() {
    local cmd="$1"
    if ! command -v "${cmd}" &>/dev/null; then
        die "'${cmd}' not found. Install LLVM (sudo apt install llvm-22) and ensure it is on PATH."
    fi
}

# ── phase 1: instrumented build and profiling run ─────────────────────────────

phase_generate() {
    print_step "Phase 1 – Instrumented build (pgo-generate preset)"

    cmake --preset pgo-generate -S "${ROOT}" 2>&1
    cmake --build --preset pgo-generate 2>&1

    mkdir -p "${PROFILES_DIR}"

    # Remove stale profraw files so the merge step is deterministic
    rm -f "${PROFILES_DIR}"/*.profraw

    print_step "Phase 1 – Running benchmarks to collect profile data"

    if [[ ! -x "${BENCH_BIN}" ]]; then
        die "Benchmark binary not found: ${BENCH_BIN}"
    fi

    # Run all benchmarks; LLVM_PROFILE_FILE drives per-process .profraw output.
    # %p expands to the PID so parallel processes don't clobber each other.
    LLVM_PROFILE_FILE="${PROFRAW_PATTERN}" \
        "${BENCH_BIN}" --benchmark_min_time=0.5 2>&1

    # Optionally also run the main application briefly to capture real UI paths
    if [[ -x "${APP_BIN}" ]]; then
        echo
        echo "Tip: You can run the main application to capture additional UI profile data:"
        echo "  LLVM_PROFILE_FILE='${PROFRAW_PATTERN}' ${APP_BIN}"
        echo "  (Use it for a few seconds, then exit.)"
    fi

    echo
    echo "Profile data written to: ${PROFILES_DIR}/*.profraw"
}

# ── phase 2: merge ────────────────────────────────────────────────────────────

phase_merge() {
    print_step "Phase 2 – Merging profraw files → ${PROFDATA}"

    require_cmd llvm-profdata

    local profraw_files=()
    while IFS= read -r -d '' f; do
        profraw_files+=("$f")
    done < <(find "${PROFILES_DIR}" -name '*.profraw' -print0 2>/dev/null)

    if [[ ${#profraw_files[@]} -eq 0 ]]; then
        die "No .profraw files found in ${PROFILES_DIR}. Run phase 1 first (./tools/pgo.sh generate)."
    fi

    echo "Merging ${#profraw_files[@]} .profraw file(s)…"
    llvm-profdata merge -sparse "${profraw_files[@]}" -o "${PROFDATA}"

    echo "Profile data merged: ${PROFDATA}"
    echo "  Size: $(du -sh "${PROFDATA}" | cut -f1)"
}

# ── phase 3: PGO-optimised build ──────────────────────────────────────────────

phase_use() {
    print_step "Phase 3 – PGO-optimised build (pgo-use preset)"

    if [[ ! -f "${PROFDATA}" ]]; then
        die "Profile data not found: ${PROFDATA}. Run merge step first (./tools/pgo.sh merge)."
    fi

    cmake --preset pgo-use -S "${ROOT}" 2>&1
    cmake --build --preset pgo-use 2>&1

    local final_bin="${ROOT}/build/pgo-use/bin/TaskSmack"
    echo
    echo "PGO-optimised binary: ${final_bin}"

    if command -v size &>/dev/null && [[ -f "${final_bin}" ]]; then
        echo "Binary size: $(du -sh "${final_bin}" | cut -f1)"
    fi
}

# ── main ──────────────────────────────────────────────────────────────────────

main() {
    local cmd="${1:-all}"

    case "${cmd}" in
        generate) phase_generate ;;
        merge)    phase_merge ;;
        use)      phase_use ;;
        all)
            phase_generate
            phase_merge
            phase_use
            print_step "PGO workflow complete"
            echo "  Instrumented binary : build/pgo-generate/bin/TaskSmack"
            echo "  Profile data        : profiles/tasksmack.profdata"
            echo "  Optimised binary    : build/pgo-use/bin/TaskSmack"
            ;;
        *)
            echo "Usage: $0 [generate|merge|use|all]"
            echo "  generate  – instrumented build + collect profile data"
            echo "  merge     – merge *.profraw files into tasksmack.profdata"
            echo "  use       – build PGO-optimised binary from tasksmack.profdata"
            echo "  all       – run all three phases in order (default)"
            exit 1
            ;;
    esac
}

main "$@"
