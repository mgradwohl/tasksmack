#!/usr/bin/env bash
# Common shell functions for TaskSmack build tools
# Source this file in other scripts: source "$(dirname "$0")/common.sh"
set -euo pipefail

# Check if a command exists and provide installation instructions if missing
# Usage: check_command "cmake" "apt install cmake"
# Returns: 0 if command exists, 1 if not found
check_command() {
    local cmd="$1"
    local install_msg="$2"
    if ! command -v "$cmd" &>/dev/null; then
        echo "Error: $cmd not found. Install via: $install_msg" >&2
        return 1
    fi
    return 0
}

# Validate build prerequisites (cmake, ninja, clang++)
# Returns: 0 if all prerequisites are met, 1 otherwise
validate_build_prereqs() {
    check_command cmake "apt install cmake" || return 1
    check_command ninja "apt install ninja-build" || return 1
    # Use the version-aware finder so the check stays valid after LLVM upgrades.
    local clangpp
    clangpp="$(find_llvm_tool "clang++" 2>/dev/null || true)"
    if [[ -z "$clangpp" ]]; then
        echo "Error: clang++ not found. Install via: apt install clang-22 lld-22 (or later)" >&2
        return 1
    fi
    local clangpp_version
    clangpp_version="$(get_llvm_tool_major_version "$clangpp" 2>/dev/null || true)"
    if [[ -z "$clangpp_version" || "$clangpp_version" -lt 22 ]]; then
        echo "Error: clang++ >= 22 required, found: ${clangpp_version:-unknown} ($clangpp)" >&2
        return 1
    fi
    return 0
}

# Validate coverage prerequisites (llvm-cov, llvm-profdata)
# Returns: 0 if all prerequisites are met, 1 otherwise
validate_coverage_prereqs() {
    check_command llvm-cov "apt install llvm" || return 1
    check_command llvm-profdata "apt install llvm" || return 1
    return 0
}

# Find LLVM tool (tries versioned paths first, then PATH)
# Usage: find_llvm_tool "clang-format"
# Returns: path to tool or empty string if not found
find_llvm_tool() {
    local tool="$1"

    # Try versioned LLVM installations first
    for ver in 22 21 20 19 18 17; do
        if [[ -x "/usr/lib/llvm-$ver/bin/$tool" ]]; then
            echo "/usr/lib/llvm-$ver/bin/$tool"
            return 0
        fi
    done

    # Fall back to PATH
    if command -v "$tool" &>/dev/null; then
        command -v "$tool"
        return 0
    fi

    return 1
}

# Print the LLVM major version for the given executable path.
get_llvm_tool_major_version() {
    local tool_path="$1"
    "$tool_path" --version 2>/dev/null | grep -oE 'version [0-9]+' | grep -oE '[0-9]+' | head -1
}

# Returns 0 if the given executable exists and is Python >= 3.14 (project minimum).
# Usage: _python_meets_min python3
_python_meets_min() {
    local exe="$1"
    command -v "$exe" &>/dev/null || return 1
    local ver
    ver="$("$exe" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')" || return 1
    local major="${ver%%.*}" minor="${ver##*.}"
    (( major > 3 || (major == 3 && minor >= 14) ))
}

# Find the first Python >= 3.14 interpreter in the project venv or PATH.
# Prints the resolved filesystem path on success; returns 1 if no qualifying interpreter found.
find_python() {
    local exe
    local common_dir repo_root
    common_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    repo_root="$(cd "${common_dir}/.." && pwd)"
    for exe in "${repo_root}/.venv/bin/python" python3.14 python3 python; do
        if _python_meets_min "$exe"; then
            command -v "$exe"
            return 0
        fi
    done
    return 1
}

# Warn when a clang-format's major version differs from the one CI pins in .pre-commit-config.yaml
# (mirrors-clang-format `rev:`). Different majors can format the same code differently, so a
# failure from check-format.sh, or a rewrite by clang-format.sh, may be the toolchain rather than
# the tree (#916). Never fails; prints to stderr only.
# Usage: warn_clang_format_version_skew "/usr/bin/clang-format"
warn_clang_format_version_skew() {
    local tool_path="$1"
    local common_dir config pinned local_version
    common_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    config="${common_dir}/../.pre-commit-config.yaml"
    [[ -f "$config" ]] || return 0
    pinned="$(grep -A1 'mirrors-clang-format' "$config" | grep -oE 'rev: *v?[0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
    local_version="$(get_llvm_tool_major_version "$tool_path" || true)"
    if [[ -n "$pinned" && -n "$local_version" && "$pinned" != "$local_version" ]]; then
        echo "Warning: $tool_path is clang-format $local_version, but CI pins clang-format $pinned (.pre-commit-config.yaml)." >&2
        echo "         Results can differ from CI; 'pre-commit run clang-format --all-files' uses the pinned version." >&2
    fi
    return 0
}

# List the files this branch changes, for the --changed-only modes of clang-format.sh and
# clang-tidy.sh: everything that differs from the merge-base with origin/main (or local main) --
# commits already on the branch, staged and unstaged edits -- plus untracked files that aren't
# ignored. `git diff HEAD` alone missed both the branch's commits and new files (#1187). Deleted
# files are left out. Falls back to HEAD when neither main ref exists. Paths are relative to the
# repository root, one per line.
# Usage: list_changed_files "/path/to/repo"
list_changed_files() {
    local root="$1"
    local base="HEAD" ref
    for ref in origin/main main; do
        if git -C "$root" rev-parse --verify --quiet "${ref}^{commit}" >/dev/null; then
            base="$(git -C "$root" merge-base HEAD "$ref" 2>/dev/null || echo HEAD)"
            break
        fi
    done
    {
        # core.quotePath=false: non-ASCII paths come out as UTF-8 rather than quoted octal escapes.
        git -C "$root" -c core.quotePath=false diff --name-only --diff-filter=d "$base" 2>/dev/null || true
        git -C "$root" -c core.quotePath=false ls-files --others --exclude-standard 2>/dev/null || true
    } | sort -u
}
