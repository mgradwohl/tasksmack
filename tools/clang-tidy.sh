#!/usr/bin/env bash
# Run clang-tidy on source files with parallel execution
# Uses .clang-tidy configuration from project root
# Usage: ./clang-tidy.sh [OPTIONS] [BUILD_TYPE] [FILES...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

escape_regex() {
    # Escape characters that are special in ERE.
    # We intentionally keep '/' unescaped on POSIX.
    printf '%s' "$1" | sed -e 's/[][(){}.^$+*?|\\]/\\&/g'
}

# Defaults
BUILD_TYPE="debug"
VERBOSE=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
FILES=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE] [FILES...]

Run clang-tidy static analysis on source files with parallel execution.
Configures if needed, strips module flags, and runs clang-tidy on all source files.

BUILD_TYPE:
  debug           Use debug build (default)
  relwithdebinfo  Use relwithdebinfo build

Options:
  -v, --verbose   Show verbose output with per-file progress
  -j, --jobs N    Number of parallel jobs (default: $JOBS)
  -h, --help      Show this help

FILES:
  Optional list of specific files to analyze (default: all src/*.cpp)

Examples:
  $(basename "$0")                          # Analyze all files
  $(basename "$0") -v -j 8                  # Verbose with 8 jobs
  $(basename "$0") src/Domain/ProcessModel.cpp  # Analyze specific file
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        -h|--help) usage ;;
        debug|relwithdebinfo) BUILD_TYPE="$1"; shift ;;
        *.cpp|*.h) FILES+=("$1"); shift ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"
COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"
CONFIG_FILE="${PROJECT_ROOT}/.clang-tidy"

# Find clang-tidy
CLANG_TIDY=$(command -v clang-tidy 2>/dev/null || echo "")
if [[ -z "$CLANG_TIDY" ]]; then
    echo "Error: clang-tidy not found in PATH" >&2
    exit 1
fi

if $VERBOSE; then
    echo "Using clang-tidy: $CLANG_TIDY"
fi

# Limit header diagnostics to project headers.
# Note: clang-tidy requires --header-filter to be set when using --exclude-header-filter.
PROJECT_ROOT_REGEX="$(escape_regex "$PROJECT_ROOT")"
HEADER_FILTER_REGEX="^${PROJECT_ROOT_REGEX}/(src|tests)/"

# Exclude generated/build trees and the other platform's folder.
# gladsources is generated under build/<preset>/gladsources.
EXCLUDE_HEADER_FILTER_REGEX="^${PROJECT_ROOT_REGEX}/(build|dist|coverage|\.cache)/|^${PROJECT_ROOT_REGEX}/src/Platform/Windows/|^${PROJECT_ROOT_REGEX}/.*/gladsources/"

# Configure if needed (using CMake presets)
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "Build not configured. Running cmake --preset $BUILD_TYPE..."
    cmake --preset "$BUILD_TYPE"
fi

# Ensure compile_commands.json exists
if [[ ! -f "$COMPILE_COMMANDS" ]]; then
    echo "Building to generate compile_commands.json..."
    cmake --build "$BUILD_DIR" --target copy-compile-commands
fi

# Strip C++20 module flags from compile_commands.json (clang-tidy doesn't handle them)
# Note: PCH flags are no longer included in compile_commands.json by CMake/Ninja
if $VERBOSE; then
    echo "Stripping module flags from compile_commands.json..."
fi
sed -i.bak \
    -e 's/@[^ ]*\.modmap//g' \
    -e 's/-fmodule-output=[^ ]*//g' \
    "$COMPILE_COMMANDS"
rm -f "${COMPILE_COMMANDS}.bak"

# Determine files to analyze
if [[ ${#FILES[@]} -eq 0 ]]; then
    # Get all source files from project, excluding other-platform files
    mapfile -t SOURCE_FILES < <(find "${PROJECT_ROOT}/src" -name "*.cpp" -type f \
        ! -path "*/Platform/Windows/*" 2>/dev/null)
else
    SOURCE_FILES=()
    for f in "${FILES[@]}"; do
        if [[ "$f" = /* ]]; then
            SOURCE_FILES+=("$f")
        else
            SOURCE_FILES+=("${PROJECT_ROOT}/$f")
        fi
    done
fi

if [[ ${#SOURCE_FILES[@]} -eq 0 ]]; then
    echo "No source files found to analyze."
    exit 0
fi

if $VERBOSE; then
    echo "Running clang-tidy on ${#SOURCE_FILES[@]} files with $JOBS parallel jobs..."
    echo ""
fi

# Function to run clang-tidy on a single file
# shellcheck disable=SC2317  # Function is called via GNU parallel/find -exec
run_clang_tidy() {
    local file="$1"
    local verbose="$2"
    local relative_path="${file#"${PROJECT_ROOT}"/}"

    if [[ "$verbose" == "true" ]]; then
        echo "  Analyzing: $relative_path"
    fi

    "$CLANG_TIDY" \
        --config-file="$CONFIG_FILE" \
        --header-filter="$HEADER_FILTER_REGEX" \
        --exclude-header-filter="$EXCLUDE_HEADER_FILTER_REGEX" \
        -p "$BUILD_DIR" \
        --extra-arg=-std=c++23 \
        --extra-arg=-Wno-unknown-warning-option \
        "$file" 2>&1
}
export -f run_clang_tidy
export CLANG_TIDY CONFIG_FILE BUILD_DIR PROJECT_ROOT HEADER_FILTER_REGEX EXCLUDE_HEADER_FILTER_REGEX

# Run clang-tidy in parallel
HAS_ERRORS=0
if command -v parallel &>/dev/null; then
    # Use GNU parallel if available
    if ! printf '%s\n' "${SOURCE_FILES[@]}" | parallel -j "$JOBS" run_clang_tidy {} "$VERBOSE"; then
        HAS_ERRORS=1
    fi
else
    # Fall back to xargs
    if ! printf '%s\0' "${SOURCE_FILES[@]}" | xargs -0 -P "$JOBS" -I {} bash -c 'run_clang_tidy "$@"' _ {} "$VERBOSE"; then
        HAS_ERRORS=1
    fi
fi

echo ""
if [[ $HAS_ERRORS -ne 0 ]]; then
    echo "clang-tidy found issues."
    exit 1
else
    echo "clang-tidy completed successfully."
    exit 0
fi
