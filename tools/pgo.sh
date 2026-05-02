#!/usr/bin/env bash
# tools/pgo.sh – End-to-end Profile-Guided Optimization workflow for Linux
#
# Usage:
#   ./tools/pgo.sh             # Full PGO workflow (generate + merge + use)
#   ./tools/pgo.sh generate    # Phase 1 only: instrumented build + run to collect data
#   ./tools/pgo.sh merge       # Phase 2 only: merge *.profraw → profiles/tasksmack.profdata
#   ./tools/pgo.sh use         # Phase 3 only: build PGO-optimized binary
#
# The resulting binary is at: build/pgo-use/bin/TaskSmack
#
# Requirements:
#   - clang++-22 (project Clang version, see CONTRIBUTING.md)
#   - llvm-profdata (ships with llvm/llvm-22; discovered via find_llvm_tool)
#   - cmake, ninja
#
# Background: Clang PGO works in four steps:
#   1. Build with -fprofile-instr-generate (instrumented binary that records branch counts)
#   2. Run the instrumented binary; LLVM_PROFILE_FILE controls output filename
#   3. Merge the per-run .profraw files into a single .profdata with llvm-profdata
#   4. Build again with -fprofile-instr-use=<path>.profdata for an optimized binary

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"
PROFILES_DIR="${ROOT}/profiles"
PROFRAW_PATTERN="${PROFILES_DIR}/tasksmack-%p.profraw"
PROFDATA="${PROFILES_DIR}/tasksmack.profdata"
BENCH_BIN="${ROOT}/build/pgo-generate/bin/TaskSmackBenchmarks"
APP_BIN="${ROOT}/build/pgo-generate/bin/TaskSmack"

# ── helpers ──────────────────────────────────────────────────────────────────

print_step() { echo; echo "──────────────────────────────────────────"; echo "  $*"; echo "──────────────────────────────────────────"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# Validate PGO prerequisites (superset of validate_build_prereqs).
# The pgo-generate/pgo-use presets require clang-22 (C compiler) and an LLD
# installation in addition to clang++-22, cmake, and ninja.
# LLD may be present as lld-22, ld.lld, or lld depending on the distro packaging.
validate_pgo_prereqs() {
    validate_build_prereqs || return 1
    check_command clang-22 "apt install clang-22" || return 1
    if ! command -v lld-22 &>/dev/null && ! command -v ld.lld &>/dev/null && ! command -v lld &>/dev/null; then
        echo "Error: LLD linker not found (tried lld-22, ld.lld, lld). Install via: apt install lld-22 (versioned) or apt install lld (unversioned)" >&2
        return 1
    fi
    # Validate llvm-profdata up front so the all/generate phases fail before the
    # expensive instrumented build rather than deep in phase_merge.
    # Use the shared find_llvm_tool helper from common.sh so any valid LLVM install
    # (versioned Debian packages, official tarballs, Homebrew, custom PATH) is accepted.
    if ! find_llvm_tool llvm-profdata &>/dev/null; then
        echo "Error: llvm-profdata not found. Install LLVM: sudo apt install llvm-22" >&2
        return 1
    fi
    return 0
}

# ── phase 1: instrumented build and profiling run ─────────────────────────────

phase_generate() {
    print_step "Phase 1 – Instrumented build (pgo-generate preset)"

    validate_pgo_prereqs || die "Missing build prerequisites. See CONTRIBUTING.md."

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
    # The KEY=value prefix scopes the assignment to the subprocess only, so any
    # pre-existing LLVM_PROFILE_FILE in the current shell is automatically preserved.
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

    # Use the shared find_llvm_tool helper from common.sh to locate llvm-profdata.
    # This accepts any valid LLVM install (versioned Debian packages, official
    # tarballs, Homebrew, custom PATH) rather than hard-coding Debian paths.
    local llvm_profdata
    llvm_profdata="$(find_llvm_tool llvm-profdata)" || \
        die "'llvm-profdata' not found. Install LLVM (e.g. sudo apt install llvm-22)."

    local profraw_files=()
    while IFS= read -r -d '' f; do
        profraw_files+=("$f")
    done < <(find "${PROFILES_DIR}" -name '*.profraw' -print0 2>/dev/null)

    if [[ ${#profraw_files[@]} -eq 0 ]]; then
        die "No .profraw files found in ${PROFILES_DIR}. Run phase 1 first (./tools/pgo.sh generate)."
    fi

    echo "Merging ${#profraw_files[@]} .profraw file(s)…"
    "${llvm_profdata}" merge -sparse "${profraw_files[@]}" -o "${PROFDATA}"

    echo "Profile data merged: ${PROFDATA}"
    if command -v du &>/dev/null; then
        echo "  Size: $(du -sh "${PROFDATA}" | cut -f1)"
    fi
}

# ── phase 3: PGO-optimized build ──────────────────────────────────────────────

phase_use() {
    print_step "Phase 3 – PGO-optimized build (pgo-use preset)"

    validate_pgo_prereqs || die "Missing build prerequisites. See CONTRIBUTING.md."

    if [[ ! -f "${PROFDATA}" ]]; then
        die "Profile data not found: ${PROFDATA}. Run merge step first (./tools/pgo.sh merge)."
    fi

    cmake --preset pgo-use -S "${ROOT}" 2>&1
    cmake --build --preset pgo-use 2>&1

    local final_bin="${ROOT}/build/pgo-use/bin/TaskSmack"
    echo
    echo "PGO-optimized binary: ${final_bin}"

    if command -v du &>/dev/null && [[ -f "${final_bin}" ]]; then
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
            echo "  Optimized binary    : build/pgo-use/bin/TaskSmack"
            ;;
        *)
            echo "Usage: $0 [generate|merge|use|all]"
            echo "  generate  – instrumented build + collect profile data"
            echo "  merge     – merge *.profraw files into tasksmack.profdata"
            echo "  use       – build PGO-optimized binary from tasksmack.profdata"
            echo "  all       – run all three phases in order (default)"
            exit 1
            ;;
    esac
}

main "$@"
