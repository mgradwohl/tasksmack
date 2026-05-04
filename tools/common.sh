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
    check_command clang++-22 "apt install clang-22 lld-22" || return 1
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

# Find the first Python >= 3.14 interpreter in PATH.
# Tries 'python3' then 'python' to handle pyenv/custom installs where either
# may point to the qualifying version.
# Prints the resolved filesystem path on success; returns 1 if no qualifying interpreter found.
find_python() {
    local exe
    for exe in python3 python; do
        if _python_meets_min "$exe"; then
            type -P "$exe"
            return 0
        fi
    done
    return 1
}
