#!/usr/bin/env bash
#
# Check Prerequisites Script
# Displays all required tools, their paths, versions, and status.
#
# Usage: ./check-prereqs.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/common.sh
source "$SCRIPT_DIR/common.sh"

# Required versions (minimum)
MIN_CMAKE_VERSION="3.29"
MIN_CLANG_VERSION="22"
MIN_CCACHE_VERSION="4.9.1"
MIN_GIT_VERSION="2.30"
MIN_PYTHON_VERSION="3.14"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Compare version strings: returns 0 if $1 >= $2
version_ge() {
    printf '%s\n%s\n' "$2" "$1" | sort -V -C
}

# Print a status line
print_status() {
    local name="$1"
    local status="$2"
    local version="$3"
    local path="$4"
    local required="$5"

    if [[ "$status" == "ok" ]]; then
        printf "${GREEN}✓${NC} ${BOLD}%-14s${NC} " "$name"
    else
        printf "${RED}✗${NC} ${BOLD}%-14s${NC} " "$name"
    fi

    if [[ -n "$version" ]]; then
        if [[ "$status" == "ok" ]]; then
            printf "${GREEN}%-12s${NC} " "$version"
        else
            printf "${RED}%-12s${NC} " "$version"
        fi
    else
        printf "${RED}%-12s${NC} " "NOT FOUND"
    fi

    if [[ -n "$required" ]]; then
        printf "${CYAN}(>= %-6s)${NC} " "$required"
    else
        printf "%-12s " ""
    fi

    if [[ -n "$path" ]]; then
        printf "${BLUE}%s${NC}" "$path"
    fi

    echo ""
}

# Get tool path
get_path() {
    command -v "$1" 2>/dev/null || echo ""
}

# Get clang version
get_clang_version() {
    if command -v clang &>/dev/null; then
        clang --version 2>/dev/null | grep -oE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get clang++ major version
get_clangpp_version() {
    if command -v clang++ &>/dev/null; then
        clang++ --version 2>/dev/null | grep -oE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get clangd version
get_clangd_version() {
    if command -v clangd &>/dev/null; then
        clangd --version 2>/dev/null | grep -oE 'clangd version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get cmake version
get_cmake_version() {
    if command -v cmake &>/dev/null; then
        cmake --version 2>/dev/null | grep -oE 'cmake version [0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
    else
        echo ""
    fi
}

# Get ccache version
get_ccache_version() {
    if command -v ccache &>/dev/null; then
        ccache --version 2>/dev/null | grep -oE 'ccache version [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get ninja version
get_ninja_version() {
    if command -v ninja &>/dev/null; then
        ninja --version 2>/dev/null | head -1
    else
        echo ""
    fi
}

# Get lld version
get_lld_version() {
    if command -v ld.lld &>/dev/null; then
        ld.lld --version 2>/dev/null | grep -oE 'LLD [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1
    elif command -v lld &>/dev/null; then
        lld --version 2>/dev/null | grep -oE 'LLD [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get clang-tidy version
get_clang_tidy_version() {
    if command -v clang-tidy &>/dev/null; then
        clang-tidy --version 2>/dev/null | grep -oE 'LLVM version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get clang-format version
get_clang_format_version() {
    if command -v clang-format &>/dev/null; then
        clang-format --version 2>/dev/null | grep -oE 'clang-format version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get llvm-profdata version
get_llvm_profdata_version() {
    if command -v llvm-profdata &>/dev/null; then
        llvm-profdata show --version 2>/dev/null | grep -oE 'LLVM version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get iwyu version
get_iwyu_version() {
    if command -v iwyu &>/dev/null; then
        iwyu --version 2>&1 | grep -oE 'include-what-you-use [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get llvm-cov version
get_llvm_cov_version() {
    if command -v llvm-cov &>/dev/null; then
        llvm-cov --version 2>/dev/null | grep -oE 'LLVM version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get git version
get_git_version() {
    if command -v git &>/dev/null; then
        git --version 2>/dev/null | grep -oE 'git version [0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
    else
        echo ""
    fi
}

# Get python3 version
get_python3_version() {
    if command -v python3 &>/dev/null; then
        python3 --version 2>/dev/null | grep -oE 'Python [0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
    elif command -v python &>/dev/null; then
        # Check if python is Python 3
        local ver
        ver=$(python --version 2>/dev/null | grep -oE 'Python [0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)
        if [[ "$ver" == 3.* ]]; then
            echo "$ver"
        else
            echo ""
        fi
    else
        echo ""
    fi
}

# Check if jinja2 Python module is installed
get_jinja2_version() {
    local py_path
    py_path="$(find_python 2>/dev/null)" || return 0

    "$py_path" - <<'EOF' 2>/dev/null
try:
    import jinja2
    print(getattr(jinja2, "__version__", "unknown"))
except Exception:
    pass
EOF
}

# Compare two version strings (returns 0 if actual >= required)
version_at_least() {
    local required="$1"
    local actual="$2"

    if [[ -z "${actual}" ]]; then
        return 1
    fi

    local first
    first="$(printf '%s\n%s\n' "${required}" "${actual}" | sort -V | head -n1)"
    if [[ "${first}" == "${required}" ]]; then
        return 0
    fi

    return 1
}

main() {
    echo -e "${BOLD}${CYAN}TaskSmack prerequisite check${NC}"
    echo

    local all_ok=0
    local missing_clangd=0
    local missing_clang_format=0

    # CMake
    local cmake_ver
    cmake_ver="$(get_cmake_version)"
    if [[ -z "${cmake_ver}" ]]; then
        echo -e "${RED}cmake${NC}: not found"
        all_ok=1
    else
        local cmake_path
        cmake_path="$(command -v cmake 2>/dev/null || true)"
        if version_at_least "${MIN_CMAKE_VERSION}" "${cmake_ver}"; then
            echo -e "${GREEN}cmake${NC}: ${cmake_ver} (${cmake_path})"
        else
            echo -e "${YELLOW}cmake${NC}: ${cmake_ver} (${cmake_path}) - minimum required ${MIN_CMAKE_VERSION}"
            all_ok=1
        fi
    fi

    # Clang
    local clang_ver
    clang_ver="$(get_clang_version)"
    if [[ -z "${clang_ver}" ]]; then
        echo -e "${RED}clang${NC}: not found"
        all_ok=1
    else
        local clang_path
        clang_path="$(command -v clang 2>/dev/null || true)"
        if version_at_least "${MIN_CLANG_VERSION}" "${clang_ver}"; then
            echo -e "${GREEN}clang${NC}: ${clang_ver} (${clang_path})"
        else
            echo -e "${YELLOW}clang${NC}: ${clang_ver} (${clang_path}) - minimum required ${MIN_CLANG_VERSION}"
            all_ok=1
        fi
    fi

    # clang++
    local clangpp_ver
    clangpp_ver="$(get_clangpp_version)"
    if [[ -z "${clangpp_ver}" ]]; then
        echo -e "${RED}clang++${NC}: not found"
        all_ok=1
    else
        local clangpp_path
        clangpp_path="$(command -v clang++ 2>/dev/null || true)"
        if version_at_least "${MIN_CLANG_VERSION}" "${clangpp_ver}"; then
            echo -e "${GREEN}clang++${NC}: ${clangpp_ver} (${clangpp_path})"
        else
            echo -e "${YELLOW}clang++${NC}: ${clangpp_ver} (${clangpp_path}) - minimum required ${MIN_CLANG_VERSION}"
            all_ok=1
        fi
    fi

    # clangd (required for VS Code workspace defaults)
    local clangd_ver
    clangd_ver="$(get_clangd_version)"
    if [[ -z "${clangd_ver}" ]]; then
        echo -e "${RED}clangd${NC}: not found (required on PATH for VS Code diagnostics)"
        all_ok=1
        missing_clangd=1
    else
        local clangd_path
        clangd_path="$(command -v clangd 2>/dev/null || true)"
        if version_at_least "${MIN_CLANG_VERSION}" "${clangd_ver}"; then
            echo -e "${GREEN}clangd${NC}: ${clangd_ver} (${clangd_path})"
        else
            echo -e "${YELLOW}clangd${NC}: ${clangd_ver} (${clangd_path}) - minimum required ${MIN_CLANG_VERSION}"
            all_ok=1
        fi
    fi

    # lld
    local lld_ver
    lld_ver="$(get_lld_version)"
    if [[ -z "${lld_ver}" ]]; then
        echo -e "${RED}lld${NC}: not found"
        all_ok=1
    else
        local lld_path
        lld_path="$(command -v ld.lld 2>/dev/null || command -v lld 2>/dev/null || true)"
        echo -e "${GREEN}lld${NC}: ${lld_ver} (${lld_path})"
    fi

    # clang-format (required for VS Code/workflow defaults)
    local clang_format_ver
    clang_format_ver="$(get_clang_format_version)"
    if [[ -z "${clang_format_ver}" ]]; then
        echo -e "${RED}clang-format${NC}: not found (required on PATH for formatting)"
        all_ok=1
        missing_clang_format=1
    else
        local clang_format_path
        clang_format_path="$(command -v clang-format 2>/dev/null || true)"
        if version_at_least "${MIN_CLANG_VERSION}" "${clang_format_ver}"; then
            echo -e "${GREEN}clang-format${NC}: ${clang_format_ver} (${clang_format_path})"
        else
            echo -e "${YELLOW}clang-format${NC}: ${clang_format_ver} (${clang_format_path}) - minimum recommended ${MIN_CLANG_VERSION}"
            all_ok=1
        fi
    fi

    # clang-tidy
    local clang_tidy_ver
    clang_tidy_ver="$(get_clang_tidy_version)"
    if [[ -z "${clang_tidy_ver}" ]]; then
        echo -e "${RED}clang-tidy${NC}: not found"
        all_ok=1
    else
        local clang_tidy_path
        clang_tidy_path="$(command -v clang-tidy 2>/dev/null || true)"
        if version_at_least "${MIN_CLANG_VERSION}" "${clang_tidy_ver}"; then
            echo -e "${GREEN}clang-tidy${NC}: ${clang_tidy_ver} (${clang_tidy_path})"
        else
            echo -e "${YELLOW}clang-tidy${NC}: ${clang_tidy_ver} (${clang_tidy_path}) - minimum required ${MIN_CLANG_VERSION}"
            all_ok=1
        fi
    fi

    # ccache
    local ccache_ver
    ccache_ver="$(get_ccache_version)"
    if [[ -z "${ccache_ver}" ]]; then
        echo -e "${RED}ccache${NC}: not found"
        all_ok=1
    else
        local ccache_path
        ccache_path="$(command -v ccache 2>/dev/null || true)"
        if version_at_least "${MIN_CCACHE_VERSION}" "${ccache_ver}"; then
            echo -e "${GREEN}ccache${NC}: ${ccache_ver} (${ccache_path})"
        else
            echo -e "${YELLOW}ccache${NC}: ${ccache_ver} (${ccache_path}) - minimum required ${MIN_CCACHE_VERSION}"
            all_ok=1
        fi
    fi

    # git
    local git_ver
    git_ver="$(get_git_version)"
    if [[ -z "${git_ver}" ]]; then
        echo -e "${RED}git${NC}: not found"
        all_ok=1
    else
        local git_path
        git_path="$(command -v git 2>/dev/null || true)"
        if version_at_least "${MIN_GIT_VERSION}" "${git_ver}"; then
            echo -e "${GREEN}git${NC}: ${git_ver} (${git_path})"
        else
            echo -e "${YELLOW}git${NC}: ${git_ver} (${git_path}) - minimum required ${MIN_GIT_VERSION}"
            all_ok=1
        fi
    fi

    # ninja
    local ninja_ver
    ninja_ver="$(get_ninja_version)"
    if [[ -z "${ninja_ver}" ]]; then
        echo -e "${RED}ninja${NC}: not found"
        all_ok=1
    else
        local ninja_path
        ninja_path="$(command -v ninja 2>/dev/null || true)"
        echo -e "${GREEN}ninja${NC}: ${ninja_ver} (${ninja_path})"
    fi

    # iwyu (optional)
    local iwyu_ver
    iwyu_ver="$(get_iwyu_version)"
    if [[ -z "${iwyu_ver}" ]]; then
        echo -e "${YELLOW}iwyu${NC}: not found (optional; required only for ./tools/iwyu.sh)"
        echo -e "  ${CYAN}Hint${NC}: sudo apt install iwyu"
    else
        local iwyu_path
        iwyu_path="$(command -v iwyu 2>/dev/null || true)"
        echo -e "${GREEN}iwyu${NC}: ${iwyu_ver} (${iwyu_path})"
    fi

    # llvm-profdata (informational)
    local llvm_profdata_ver
    llvm_profdata_ver="$(get_llvm_profdata_version)"
    if [[ -z "${llvm_profdata_ver}" ]]; then
        echo -e "${YELLOW}llvm-profdata${NC}: not found (only required for coverage)"
    else
        local llvm_profdata_path
        llvm_profdata_path="$(command -v llvm-profdata 2>/dev/null || true)"
        echo -e "${GREEN}llvm-profdata${NC}: ${llvm_profdata_ver} (${llvm_profdata_path})"
    fi

    # llvm-cov (informational)
    local llvm_cov_ver
    llvm_cov_ver="$(get_llvm_cov_version)"
    if [[ -z "${llvm_cov_ver}" ]]; then
        echo -e "${YELLOW}llvm-cov${NC}: not found (only required for coverage)"
    else
        local llvm_cov_path
        llvm_cov_path="$(command -v llvm-cov 2>/dev/null || true)"
        echo -e "${GREEN}llvm-cov${NC}: ${llvm_cov_ver} (${llvm_cov_path})"
    fi

    # Python 3 (required for GLAD OpenGL loader generation)
    local py_ver
    py_ver="$(get_python3_version)"
    if [[ -z "${py_ver}" ]]; then
        echo -e "${RED}python3${NC}: not found"
        all_ok=1
    else
        # Use find_python() for the canonical path (>= 3.14); fall back to any python3/python
        # for display when the found version is too old.
        local py_path
        py_path="$(find_python 2>/dev/null || true)"
        if [[ -z "${py_path}" ]]; then
            py_path="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
        fi
        local py_cmd
        py_cmd="$(basename "${py_path:-python3}")"
        if version_at_least "${MIN_PYTHON_VERSION}" "${py_ver}"; then
            echo -e "${GREEN}${py_cmd}${NC}: ${py_ver} (${py_path})"
        else
            echo -e "${YELLOW}${py_cmd}${NC}: ${py_ver} (${py_path}) - minimum required ${MIN_PYTHON_VERSION}"
            all_ok=1
        fi
    fi

    # jinja2 (required for GLAD OpenGL loader generation)
    local jinja_ver
    jinja_ver="$(get_jinja2_version)"
    if [[ -z "${jinja_ver}" ]]; then
        # Distinguish between "Python not found/too old" and "jinja2 not installed":
        # if find_python fails here, the Python check above already flagged the issue.
        if ! find_python &>/dev/null; then
            echo -e "${YELLOW}jinja2${NC}: skipped (requires Python >= ${MIN_PYTHON_VERSION})"
        else
            echo -e "${RED}jinja2${NC}: Python module not found (pip install jinja2)"
            all_ok=1
        fi
    else
        echo -e "${GREEN}jinja2${NC}: ${jinja_ver}"
    fi

    echo
    if [[ "${all_ok}" -eq 0 ]]; then
        echo -e "${GREEN}All mandatory prerequisites are satisfied.${NC}"
        return 0
    else
        echo -e "${RED}Some mandatory prerequisites are missing or out of date.${NC}"
        if [[ "${missing_clangd}" -eq 1 ]]; then
            local versioned_clangd
            versioned_clangd="$(command -v "clangd-${MIN_CLANG_VERSION}" 2>/dev/null || true)"
            if [[ -n "${versioned_clangd}" ]]; then
                echo -e "${YELLOW}Hint${NC}: found ${versioned_clangd}. Add an unversioned 'clangd' on PATH (e.g., via update-alternatives)."
            fi
        fi
        if [[ "${missing_clang_format}" -eq 1 ]]; then
            local versioned_clang_format
            versioned_clang_format="$(command -v "clang-format-${MIN_CLANG_VERSION}" 2>/dev/null || true)"
            if [[ -n "${versioned_clang_format}" ]]; then
                echo -e "${YELLOW}Hint${NC}: found ${versioned_clang_format}. Add an unversioned 'clang-format' on PATH (e.g., via update-alternatives)."
            fi
        fi
        return 1
    fi
}

if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    main "$@"
fi
