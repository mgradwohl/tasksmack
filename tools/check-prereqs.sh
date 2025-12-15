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

# Main
echo ""
echo -e "${BOLD}========================================"
echo -e "  Prerequisite Check"
echo -e "========================================${NC}"
echo ""
echo -e "${BOLD}Tool           Version      Required     Path${NC}"
echo "──────────────────────────────────────────────────────────────────────────"

ALL_OK=true

# Check Clang
clang_ver=$(get_clang_version)
clang_path=$(get_path clang)
if [[ -n "$clang_ver" ]] && [[ "$clang_ver" -ge "${MIN_CLANG_VERSION%%.*}" ]]; then
    print_status "clang" "ok" "$clang_ver" "$clang_path" "$MIN_CLANG_VERSION"
else
    print_status "clang" "fail" "$clang_ver" "$clang_path" "$MIN_CLANG_VERSION"
    ALL_OK=false
fi

# Check Clang++
clangpp_ver=$(get_clang_version)
clangpp_path=$(get_path clang++)
if [[ -n "$clangpp_ver" ]] && [[ "$clangpp_ver" -ge "${MIN_CLANG_VERSION%%.*}" ]]; then
    print_status "clang++" "ok" "$clangpp_ver" "$clangpp_path" "$MIN_CLANG_VERSION"
else
    print_status "clang++" "fail" "$clangpp_ver" "$clangpp_path" "$MIN_CLANG_VERSION"
    ALL_OK=false
fi

# Check CMake
cmake_ver=$(get_cmake_version)
cmake_path=$(get_path cmake)
if [[ -n "$cmake_ver" ]] && version_ge "$cmake_ver" "$MIN_CMAKE_VERSION"; then
    print_status "cmake" "ok" "$cmake_ver" "$cmake_path" "$MIN_CMAKE_VERSION"
else
    print_status "cmake" "fail" "$cmake_ver" "$cmake_path" "$MIN_CMAKE_VERSION"
    ALL_OK=false
fi

# Check Ninja
ninja_ver=$(get_ninja_version)
ninja_path=$(get_path ninja)
if [[ -n "$ninja_ver" ]]; then
    print_status "ninja" "ok" "$ninja_ver" "$ninja_path" ""
else
    print_status "ninja" "fail" "" "$ninja_path" ""
    ALL_OK=false
fi

# Check lld
lld_ver=$(get_lld_version)
lld_path=$(get_path ld.lld)
[[ -z "$lld_path" ]] && lld_path=$(get_path lld)
if [[ -n "$lld_ver" ]]; then
    print_status "lld" "ok" "$lld_ver" "$lld_path" ""
else
    print_status "lld" "fail" "" "$lld_path" ""
    ALL_OK=false
fi

# Check ccache
ccache_ver=$(get_ccache_version)
ccache_path=$(get_path ccache)
if [[ -n "$ccache_ver" ]] && version_ge "$ccache_ver" "$MIN_CCACHE_VERSION"; then
    print_status "ccache" "ok" "$ccache_ver" "$ccache_path" "$MIN_CCACHE_VERSION"
else
    print_status "ccache" "fail" "$ccache_ver" "$ccache_path" "$MIN_CCACHE_VERSION"
    ALL_OK=false
fi

# Check clang-tidy
tidy_ver=$(get_clang_tidy_version)
tidy_path=$(get_path clang-tidy)
if [[ -n "$tidy_ver" ]]; then
    print_status "clang-tidy" "ok" "$tidy_ver" "$tidy_path" ""
else
    print_status "clang-tidy" "fail" "" "$tidy_path" ""
    ALL_OK=false
fi

# Check clang-format
format_ver=$(get_clang_format_version)
format_path=$(get_path clang-format)
if [[ -n "$format_ver" ]]; then
    print_status "clang-format" "ok" "$format_ver" "$format_path" ""
else
    print_status "clang-format" "fail" "" "$format_path" ""
    ALL_OK=false
fi

# Check llvm-profdata
profdata_ver=$(get_llvm_profdata_version)
profdata_path=$(get_path llvm-profdata)
if [[ -n "$profdata_ver" ]]; then
    print_status "llvm-profdata" "ok" "$profdata_ver" "$profdata_path" ""
else
    print_status "llvm-profdata" "fail" "" "$profdata_path" ""
    # Don't fail for optional coverage tools
fi

# Check llvm-cov
cov_ver=$(get_llvm_cov_version)
cov_path=$(get_path llvm-cov)
if [[ -n "$cov_ver" ]]; then
    print_status "llvm-cov" "ok" "$cov_ver" "$cov_path" ""
else
    print_status "llvm-cov" "fail" "" "$cov_path" ""
    # Don't fail for optional coverage tools
fi

echo "──────────────────────────────────────────────────────────────────────────"
echo ""

# Environment variables
echo -e "${BOLD}Environment Variables${NC}"
echo "──────────────────────────────────────────────────────────────────────────"
if [[ -n "${CMAKE_ROOT:-}" ]]; then
    echo -e "${GREEN}✓${NC} CMAKE_ROOT      = ${BLUE}$CMAKE_ROOT${NC}"
else
    echo -e "${YELLOW}○${NC} CMAKE_ROOT      = ${YELLOW}(not set)${NC}"
fi
if [[ -n "${LLVM_ROOT:-}" ]]; then
    echo -e "${GREEN}✓${NC} LLVM_ROOT       = ${BLUE}$LLVM_ROOT${NC}"
else
    echo -e "${YELLOW}○${NC} LLVM_ROOT       = ${YELLOW}(not set)${NC}"
fi
if [[ -n "${CC:-}" ]]; then
    echo -e "${GREEN}✓${NC} CC              = ${BLUE}$CC${NC}"
else
    echo -e "${YELLOW}○${NC} CC              = ${YELLOW}(not set, using default)${NC}"
fi
if [[ -n "${CXX:-}" ]]; then
    echo -e "${GREEN}✓${NC} CXX             = ${BLUE}$CXX${NC}"
else
    echo -e "${YELLOW}○${NC} CXX             = ${YELLOW}(not set, using default)${NC}"
fi
echo "──────────────────────────────────────────────────────────────────────────"
echo ""

# Summary
if $ALL_OK; then
    echo -e "${GREEN}${BOLD}All required prerequisites are installed and meet version requirements.${NC}"
    exit 0
else
    echo -e "${RED}${BOLD}Some prerequisites are missing or do not meet version requirements.${NC}"
    echo -e "Run ${CYAN}./setup.sh --help${NC} for installation instructions."
    exit 1
fi
