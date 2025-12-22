#!/usr/bin/env bash
#
# Check Prerequisites Script
# Displays all required tools, their paths, versions, and status.
#
# Usage: ./check-prereqs.sh
#
set -euo pipefail

# Required versions (minimum)
MIN_CMAKE_VERSION="3.28"
MIN_CLANG_VERSION="22"
MIN_CCACHE_VERSION="4.9.1"
MIN_GIT_VERSION="2.30"

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

# Get working Python 3 command
get_python_cmd() {
    if command -v python3 &>/dev/null; then
        echo "python3"
    elif command -v python &>/dev/null; then
        # Check if python is Python 3
        local ver
        ver=$(python --version 2>/dev/null | grep -oE 'Python [0-9]+' | grep -oE '[0-9]+' | head -1)
        if [[ "$ver" == "3" ]]; then
            echo "python"
        else
            echo ""
        fi
    else
        echo ""
    fi
}

# Check if jinja2 Python module is installed
get_jinja2_version() {
    local py_cmd
    py_cmd="$(get_python_cmd)"

    if [[ -z "${py_cmd}" ]]; then
        echo ""
        return 1
    fi

    "${py_cmd}" - <<'EOF' 2>/dev/null
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

    # Python 3 (informational)
    local py_ver
    py_ver="$(get_python3_version)"
    if [[ -z "${py_ver}" ]]; then
        echo -e "${YELLOW}python3${NC}: not found (required for some tooling)"
    else
        local py_cmd
        py_cmd="$(get_python_cmd)"
        local py_path
        py_path="$(command -v "${py_cmd}" 2>/dev/null || true)"
        echo -e "${GREEN}${py_cmd}${NC}: ${py_ver} (${py_path})"
    fi

    # jinja2 (informational)
    local jinja_ver
    jinja_ver="$(get_jinja2_version)"
    if [[ -z "${jinja_ver}" ]]; then
        echo -e "${YELLOW}jinja2${NC}: Python module not found (required for GLAD generation)"
    else
        echo -e "${GREEN}jinja2${NC}: ${jinja_ver}"
    fi

    echo
    if [[ "${all_ok}" -eq 0 ]]; then
        echo -e "${GREEN}All mandatory prerequisites are satisfied.${NC}"
        return 0
    else
        echo -e "${RED}Some mandatory prerequisites are missing or out of date.${NC}"
        return 1
    fi
}

if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    main "$@"
fi
