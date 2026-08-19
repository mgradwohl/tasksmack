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
CHANGED_ONLY=false
FILES=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE] [FILES...]

Run clang-tidy static analysis on source files with parallel execution.
Configures if needed, strips module and PCH flags, and runs clang-tidy on all source files.

BUILD_TYPE:
  debug           Use debug build (default)
  relwithdebinfo  Use relwithdebinfo build

Options:
  -v, --verbose     Show verbose output with per-file progress
  -j, --jobs N      Number of parallel jobs (default: $JOBS)
  -c, --changed-only Only analyze changed C++ files (uses git diff)
  -h, --help        Show this help

FILES:
  Optional list of specific files to analyze (default: all src/*.cpp)

Examples:
  $(basename "$0")                          # Analyze all files
  $(basename "$0") -v -j 8                  # Verbose with 8 jobs
  $(basename "$0") --changed-only           # Only analyze changed files
  $(basename "$0") src/Domain/ProcessModel.cpp  # Analyze specific file
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        -c|--changed-only) CHANGED_ONLY=true; shift ;;
        -h|--help) usage ;;
        debug|relwithdebinfo) BUILD_TYPE="$1"; shift ;;
        *.cpp|*.h) FILES+=("$1"); shift ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"
COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"
TIDY_COMPDB_DIR="${BUILD_DIR}/clang-tidy-compdb"
COMPILE_COMMANDS_TIDY="${TIDY_COMPDB_DIR}/compile_commands.json"
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

# Build the clean tidy database on every invocation so it stays in sync with the active build.
# The generate-clang-tidy-compile-commands target writes a sanitized compile_commands.json into
# ${BUILD_DIR}/clang-tidy-compdb without touching the live database clangd watches.
if $VERBOSE; then
    echo "Generating clang-tidy compilation database via CMake target..."
fi
rm -f "$COMPILE_COMMANDS_TIDY"
cmake --build "$BUILD_DIR" --target generate-clang-tidy-compile-commands 2>/dev/null || true

# If the CMake target didn't produce the file (older build dir or no CLANG_TIDY_EXE at configure
# time), fall back to generating it here from the live database.
if [[ ! -f "$COMPILE_COMMANDS_TIDY" ]]; then
    if $VERBOSE; then
        echo "Falling back: writing sanitized compile_commands.json for clang-tidy"
    fi
    mkdir -p "$TIDY_COMPDB_DIR"
    sed \
        -e 's/@[^ ]*\.modmap//g' \
        -e 's/-fmodule-output=[^ ]*//g' \
        -e 's/-Xclang -include-pch -Xclang [^ ]*//g' \
        -e 's/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g' \
        -e 's/-Xclang -fno-pch-timestamp//g' \
        "$COMPILE_COMMANDS" > "$COMPILE_COMMANDS_TIDY"
fi

COMPILE_COMMANDS_IN_USE="$COMPILE_COMMANDS_TIDY"

# Determine files to analyze
if [[ ${#FILES[@]} -eq 0 ]]; then
    if [[ "$CHANGED_ONLY" == "true" ]]; then
        # Get changed files from git
        mapfile -t CHANGED_FILES < <(git diff --name-only HEAD 2>/dev/null | grep -E '\.(cpp|h)$' | grep -v 'Platform/Windows/' || true)
        if [[ ${#CHANGED_FILES[@]} -eq 0 ]]; then
            echo "No changed C++ files found."
            exit 0
        fi
        # Convert to absolute paths
        SOURCE_FILES=()
        for f in "${CHANGED_FILES[@]}"; do
            SOURCE_FILES+=("${PROJECT_ROOT}/$f")
        done
        if $VERBOSE; then
            echo "Analyzing ${#SOURCE_FILES[@]} changed files..."
        fi
    else
        # Get all source files from project, excluding other-platform files
        mapfile -t SOURCE_FILES < <(find "${PROJECT_ROOT}/src" -name "*.cpp" -type f \
            ! -path "*/Platform/Windows/*" 2>/dev/null)
    fi
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
    echo "Error: No source files found to analyze." >&2
    exit 1
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
        -p "$COMPILE_COMMANDS_IN_USE" \
        --extra-arg=-std=c++23 \
        --extra-arg=-Wno-unknown-warning-option \
        "$file" 2>&1
}
export -f run_clang_tidy
export CLANG_TIDY CONFIG_FILE BUILD_DIR PROJECT_ROOT HEADER_FILTER_REGEX EXCLUDE_HEADER_FILTER_REGEX COMPILE_COMMANDS_IN_USE

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
