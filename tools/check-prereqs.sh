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
MIN_CLANG_VERSION="23"
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

# Get ccache version: two or three components (4.14, 4.9.1). ccache is optional, so output that
# does not parse, or a failing ccache, prints nothing rather than fail the pipeline, which under
# set -euo pipefail would end the whole check before the summary (#2350).
get_ccache_version() {
    if command -v ccache &>/dev/null; then
        { ccache --version 2>/dev/null || true; } | grep -oE 'ccache version [0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true
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
# The ld.lld this script reports, found as find_llvm_tool() finds every other LLVM tool (#4705): the newest
# /usr/lib/llvm-N/bin first (clang -fuse-ld=lld runs the one beside it), then PATH, then /usr/bin/ld.lld-23. PATH first
# reported a /usr/local/bin shadow instead. A bare `lld` is the generic driver, which prints no version.
get_lld_path() {
    find_llvm_tool ld.lld 2>/dev/null || command -v "ld.lld-${MIN_CLANG_VERSION}" 2>/dev/null || true
}

get_lld_version() {
    local lld
    lld="$(get_lld_path)"
    if [[ -n "$lld" ]]; then
        "$lld" --version 2>/dev/null | grep -oE 'LLD [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1
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

# How to get an IWYU this project can use (#4708), for both the not-found and the wrong-Clang rows: built from source
# against Clang 23 at the commit CI pins (.github/workflows/ci.yml's IWYU_COMMIT_SHA), so a local run matches CI's.
# apt's iwyu is built against an older Clang (19 on Ubuntu 26.04), which tools/iwyu.sh refuses for --fix.
print_iwyu_hint() {
    local sha
    sha="$(grep -oE "IWYU_COMMIT_SHA: '[0-9a-f]{40}'" "$SCRIPT_DIR/../.github/workflows/ci.yml" 2>/dev/null | grep -oE '[0-9a-f]{40}' | head -1 || true)"
    echo -e "  ${CYAN}Hint${NC}: build it from source against Clang ${MIN_CLANG_VERSION}: the clang_${MIN_CLANG_VERSION} branch of"
    local at=""
    [[ -n "$sha" ]] && at=" at the commit CI pins, ${sha}"
    echo -e "        https://github.com/include-what-you-use/include-what-you-use${at}"
    echo -e "        (CONTRIBUTING.md, Include-What-You-Use, has the commands; apt's iwyu uses an older Clang)"
}

# One LLVM tool's row (#4714), as check-prereqs.ps1's Write-LlvmToolStatus: every LLVM tool reports through it, so none
# can lose its floor check again (lld did, #4705; llvm-profdata and llvm-cov never had one). Prints:
#   green   at or above MIN_CLANG_VERSION and on the major CI pins (pinned_llvm_major, .pre-commit-config.yaml);
#   yellow  at or above the floor but another major: "CI pins LLVM N" (a warning; returns 2);
#   yellow  below the floor: "minimum required N" ("recommended" for level recommended), or MISMATCH when given;
#   red     not found, " (MISSING)" after it; yellow for an optional tool.
# Below the floor or missing sets main's all_ok=1, except for an optional tool. Returns 1 then.
# Usage: report_llvm_tool NAME SHOWN_VERSION MAJOR PATH LEVEL(required|recommended|optional) [MISSING] [MISMATCH]
report_llvm_tool() {
    local name="$1" shown="$2" major="$3" path="$4" level="$5" missing="${6:-}" mismatch="${7:-}"
    if [[ -z "${shown}" ]]; then
        if [[ "${level}" == optional ]]; then
            echo -e "${YELLOW}${name}${NC}: not found${missing:+ (${missing})}"
        else
            echo -e "${RED}${name}${NC}: not found${missing:+ (${missing})}"
            all_ok=1
        fi
        return 1
    fi
    if [[ -z "${major}" ]] || ! version_at_least "${MIN_CLANG_VERSION}" "${major}"; then
        local word=required
        [[ "${level}" == recommended ]] && word=recommended
        echo -e "${YELLOW}${name}${NC}: ${shown} (${path}) - ${mismatch:-minimum ${word} ${MIN_CLANG_VERSION}}"
        [[ "${level}" == optional ]] || all_ok=1
        return 1
    fi
    if [[ -n "${PINNED_LLVM_MAJOR}" && "${major}" != "${PINNED_LLVM_MAJOR}" ]]; then
        echo -e "${YELLOW}${name}${NC}: ${shown} (${path}) - CI pins LLVM ${PINNED_LLVM_MAJOR}"
        llvm_skew_seen=1
        return 2
    fi
    echo -e "${GREEN}${name}${NC}: ${shown} (${path})"
    return 0
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
    local llvm_skew_seen=0
    # The LLVM major CI pins (#4714): a tool at the floor but another major reads as skew, not as OK.
    PINNED_LLVM_MAJOR="$(pinned_llvm_major)"

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

    # Clang, clang++ (#4714: through report_llvm_tool)
    local clang_ver clangpp_ver
    clang_ver="$(get_clang_version)"
    report_llvm_tool clang "${clang_ver}" "${clang_ver}" "$(command -v clang 2>/dev/null || true)" required || true
    clangpp_ver="$(get_clangpp_version)"
    report_llvm_tool clang++ "${clangpp_ver}" "${clangpp_ver}" "$(command -v clang++ 2>/dev/null || true)" required || true

    # The Linux presets name their compilers clang-23/clang++-23 and build against libc++, so the
    # unversioned clang above passing is not enough for `cmake --preset debug` to work.
    local preset_cc="clang-${MIN_CLANG_VERSION}" preset_cxx="clang++-${MIN_CLANG_VERSION}"
    if command -v "${preset_cc}" &>/dev/null && command -v "${preset_cxx}" &>/dev/null; then
        echo -e "${GREEN}${preset_cxx}${NC}: found ($(command -v "${preset_cxx}"); used by the CMake presets)"
        if echo '#include <version>' | "${preset_cxx}" -stdlib=libc++ -x c++ -E - &>/dev/null; then
            echo -e "${GREEN}libc++${NC}: found"
        else
            echo -e "${RED}libc++${NC}: not found (sudo apt install libc++-${MIN_CLANG_VERSION}-dev libc++abi-${MIN_CLANG_VERSION}-dev)"
            all_ok=1
        fi
    else
        echo -e "${RED}${preset_cxx}${NC}: not found (the CMake presets call ${preset_cc}/${preset_cxx}; sudo apt install clang-${MIN_CLANG_VERSION})"
        all_ok=1
    fi

    # clangd (required for VS Code workspace defaults), lld, clang-format (VS Code and workflow defaults), clang-tidy
    local clangd_ver lld_ver lld_major clang_format_ver clang_tidy_ver
    clangd_ver="$(get_clangd_version)"
    report_llvm_tool clangd "${clangd_ver}" "${clangd_ver}" "$(command -v clangd 2>/dev/null || true)" required \
        "required on PATH for VS Code diagnostics" || true
    [[ -n "${clangd_ver}" ]] || missing_clangd=1
    # lld shows major.minor (23.1); a stale one links the build's LLVM 23 objects (#4705).
    lld_ver="$(get_lld_version)"
    lld_major="${lld_ver%%.*}"
    report_llvm_tool lld "${lld_ver}" "${lld_major}" "$(get_lld_path)" required || true
    clang_format_ver="$(get_clang_format_version)"
    report_llvm_tool clang-format "${clang_format_ver}" "${clang_format_ver}" "$(command -v clang-format 2>/dev/null || true)" \
        recommended "required on PATH for formatting" || true
    [[ -n "${clang_format_ver}" ]] || missing_clang_format=1
    clang_tidy_ver="$(get_clang_tidy_version)"
    report_llvm_tool clang-tidy "${clang_tidy_ver}" "${clang_tidy_ver}" "$(command -v clang-tidy 2>/dev/null || true)" required || true

    # ccache (recommended, not required: CONTRIBUTING.md lists it as "recommended for faster rebuilds",
    # cmake/CompilerOptions.cmake only warns when neither ccache nor sccache is found, and
    # check-prereqs.ps1 warns too, #2270). Missing or too old is a warning; all_ok is left alone.
    local ccache_ver ccache_path
    ccache_ver="$(get_ccache_version)" || ccache_ver=""
    ccache_path="$(command -v ccache 2>/dev/null || true)"
    if [[ -z "${ccache_path}" ]]; then
        echo -e "${YELLOW}ccache${NC}: not found (recommended for faster rebuilds)"
        echo -e "  ${CYAN}Hint${NC}: sudo apt install ccache"
    elif [[ -z "${ccache_ver}" ]]; then
        echo -e "${YELLOW}ccache${NC}: version not recognized (${ccache_path}) - ${MIN_CCACHE_VERSION}+ recommended"
    else
        if version_at_least "${MIN_CCACHE_VERSION}" "${ccache_ver}"; then
            echo -e "${GREEN}ccache${NC}: ${ccache_ver} (${ccache_path})"
        else
            echo -e "${YELLOW}ccache${NC}: ${ccache_ver} (${ccache_path}) - ${MIN_CCACHE_VERSION}+ recommended"
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

    # xvfb-run (optional): runs the display tests on a virtual screen, as Linux CI does, so a stray click
    # or a desktop that steals focus can't change their result (#2221)
    if command -v xvfb-run >/dev/null 2>&1 && command -v xauth >/dev/null 2>&1; then
        echo -e "${GREEN}xvfb-run${NC}: found ($(command -v xvfb-run))"
    else
        echo -e "${YELLOW}xvfb-run${NC}: not found (optional; runs the display tests headless as CI does: xvfb-run -a ctest --preset debug)"
        echo -e "  ${CYAN}Hint${NC}: sudo apt install xvfb (tools/setup-dev.sh installs it)"
    fi

    # iwyu (optional): its own version shown, judged by the Clang it was built against, which must be CI's (tools/iwyu.sh
    # asserts with another major); the hint says how to get one (#4708).
    local iwyu_ver iwyu_clang
    iwyu_ver="$(get_iwyu_version)"
    iwyu_clang=""
    if [[ -n "${iwyu_ver}" ]]; then
        iwyu_clang="$(iwyu --version 2>&1 | grep -oE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
    fi
    # An IWYU whose Clang can't be read is shown as it was before #4714: at the floor.
    if ! report_llvm_tool iwyu "${iwyu_ver}" "${iwyu_clang:-${MIN_CLANG_VERSION}}" "$(command -v iwyu 2>/dev/null || true)" optional \
        "optional; required only for ./tools/iwyu.sh" \
        "built against Clang ${iwyu_clang}; CI builds it against Clang ${MIN_CLANG_VERSION}"; then
        print_iwyu_hint # what to do, not only what's wrong (#4708)
    fi

    # llvm-profdata, llvm-cov (informational: only coverage needs them, so below the floor is a warning, #4714)
    local llvm_profdata_ver llvm_cov_ver
    llvm_profdata_ver="$(get_llvm_profdata_version)"
    report_llvm_tool llvm-profdata "${llvm_profdata_ver}" "${llvm_profdata_ver}" "$(command -v llvm-profdata 2>/dev/null || true)" \
        optional "only required for coverage" || true
    llvm_cov_ver="$(get_llvm_cov_version)"
    report_llvm_tool llvm-cov "${llvm_cov_ver}" "${llvm_cov_ver}" "$(command -v llvm-cov 2>/dev/null || true)" \
        optional "only required for coverage" || true

    # ── Linux profiling tools (all optional) ────────────────────────────────
    echo
    echo "Linux profiling tools (optional — used by tools/profile-perf.sh, analyze-perf.sh, profile-heap.sh):"

    # perf — CPU sampling (profile-perf.sh, analyze-perf.sh)
    local perf_ver perf_warn
    perf_ver="$(perf --version 2>/dev/null | awk '{print $NF}' || true)"
    perf_warn="$(perf --version 2>&1 | grep -i "not found for kernel" || true)"
    if [[ -n "${perf_warn}" ]]; then
        local kernel_ver
        kernel_ver="$(uname -r)"
        echo -e "${YELLOW}perf${NC}: installed but kernel/tools version mismatch (running ${kernel_ver})"
        echo -e "       Fix: sudo apt install linux-tools-$(uname -r) linux-tools-generic"
    elif [[ -n "${perf_ver}" ]]; then
        local perf_path
        perf_path="$(command -v perf 2>/dev/null || true)"
        echo -e "${GREEN}perf${NC}: ${perf_ver} (${perf_path})"
    else
        echo -e "${YELLOW}perf${NC}: not found (sudo apt install linux-tools-generic)"
    fi

    # flamegraph.pl + stackcollapse-perf.pl — SVG flamegraphs (analyze-perf.sh)
    local flamegraph_path stackcollapse_path
    flamegraph_path="$(command -v flamegraph.pl 2>/dev/null || \
        { [[ -x "${HOME}/opt/FlameGraph/flamegraph.pl" ]] && echo "${HOME}/opt/FlameGraph/flamegraph.pl"; } || true)"
    stackcollapse_path="$(command -v stackcollapse-perf.pl 2>/dev/null || \
        { [[ -x "${HOME}/opt/FlameGraph/stackcollapse-perf.pl" ]] && echo "${HOME}/opt/FlameGraph/stackcollapse-perf.pl"; } || true)"
    if [[ -n "${flamegraph_path}" && -n "${stackcollapse_path}" ]]; then
        echo -e "${GREEN}flamegraph.pl${NC}: ${flamegraph_path}"
        echo -e "${GREEN}stackcollapse-perf.pl${NC}: ${stackcollapse_path}"
    else
        echo -e "${YELLOW}FlameGraph${NC}: not found (git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph)"
    fi

    # hotspot — GUI perf viewer (analyze-perf.sh optional launch)
    local hotspot_path
    hotspot_path="$(command -v hotspot 2>/dev/null || true)"
    # Not packaged for Ubuntu 26.04 (#4706): KDAB ships it on Flathub and as an AppImage.
    if [[ -n "${hotspot_path}" ]]; then
        echo -e "${GREEN}hotspot${NC}: ${hotspot_path}"
    elif command -v flatpak &>/dev/null && flatpak info com.kdab.hotspot &>/dev/null; then
        echo -e "${GREEN}hotspot${NC}: Flatpak com.kdab.hotspot (flatpak run com.kdab.hotspot <file>)"
    else
        echo -e "${YELLOW}hotspot${NC}: not found (not packaged for Ubuntu 26.04: flatpak install flathub com.kdab.hotspot, or KDAB's AppImage from https://github.com/KDAB/hotspot/releases saved as hotspot on PATH)"
    fi

    # heaptrack — heap allocation profiler (profile-heap.sh)
    local heaptrack_path
    heaptrack_path="$(command -v heaptrack 2>/dev/null || true)"
    if [[ -n "${heaptrack_path}" ]]; then
        local heaptrack_ver
        heaptrack_ver="$(heaptrack --version 2>/dev/null | head -1 || echo "available")"
        echo -e "${GREEN}heaptrack${NC}: ${heaptrack_ver} (${heaptrack_path})"
    else
        echo -e "${YELLOW}heaptrack${NC}: not found (sudo apt install heaptrack)"
    fi

    # heaptrack_print — headless heaptrack analysis (profile-heap.sh)
    local heaptrack_print_path
    heaptrack_print_path="$(command -v heaptrack_print 2>/dev/null || true)"
    if [[ -n "${heaptrack_print_path}" ]]; then
        echo -e "${GREEN}heaptrack_print${NC}: ${heaptrack_print_path}"
    else
        echo -e "${YELLOW}heaptrack_print${NC}: not found (ships with heaptrack)"
    fi

    # Python 3 (required for GLAD OpenGL loader generation)
    # find_python() gives the interpreter the build uses (.venv, python3.14, ... >= 3.14); report
    # that one's version. Only when there is none, fall back to whatever python3/python is on PATH.
    local py_ver py_path
    py_path="$(find_python 2>/dev/null || true)"
    if [[ -n "${py_path}" ]]; then
        py_ver="$("${py_path}" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null || true)"
    else
        py_ver="$(get_python3_version)"
        py_path="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
    fi
    if [[ -z "${py_ver}" ]]; then
        echo -e "${RED}python3${NC}: not found"
        all_ok=1
    else
        local py_cmd
        py_cmd="${py_path:-python3}"
        py_cmd="${py_cmd##*/}" # no basename: the checker must run with only the tools it checks for on PATH
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
            echo -e "${RED}jinja2${NC}: Python module not found (.venv/bin/python -m pip install --require-hashes -r requirements-glad.txt, or run tools/setup-dev.sh)"
            all_ok=1
        fi
    else
        echo -e "${GREEN}jinja2${NC}: ${jinja_ver}"
    fi

    # The libraries the Linux build links (#3915), found with pkg-config as CMake finds them: libbpf is required since
    # #3540 (configure fails without it), libsystemd and libcurl are optional and turn features off when missing.
    if command -v pkg-config &>/dev/null; then
        local entry lib pkg kind
        for entry in "libbpf:libbpf-dev:required" "libsystemd:libsystemd-dev:optional" "libcurl:libcurl4-openssl-dev:optional"; do
            IFS=: read -r lib pkg kind <<<"${entry}"
            if pkg-config --exists "${lib}"; then
                echo -e "${GREEN}${lib}${NC}: $(pkg-config --modversion "${lib}" 2>/dev/null || echo found)"
            elif [[ "${kind}" == required ]]; then
                echo -e "${RED}${lib}${NC}: not found (sudo apt install ${pkg}) - configure fails without it"
                all_ok=1
            else
                echo -e "${YELLOW}${lib}${NC}: not found (sudo apt install ${pkg}) - optional; the features it serves are turned off"
            fi
        done
    else
        echo -e "${RED}pkg-config${NC}: not found (sudo apt install pkg-config) - configure needs it to find libbpf"
        all_ok=1
    fi

    # Skew is a warning, as check-prereqs.ps1 says it: which rows, and why it matters (#4714, #916).
    if [[ "${llvm_skew_seen}" -eq 1 ]]; then
        echo
        echo -e "${YELLOW}LLVM ${PINNED_LLVM_MAJOR} is the version CI pins (.pre-commit-config.yaml); the rows marked \"CI pins\" are another major,"
        echo -e "so their formatting and clang-tidy findings can differ from CI's.${NC}"
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
