#!/usr/bin/env bash
# Run clang-tidy on source files
# Uses .clang-tidy configuration from project root
# Usage: ./clang-tidy.sh [OPTIONS] [BUILD_TYPE]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Defaults
BUILD_TYPE="debug"
VERBOSE=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE]

Run clang-tidy static analysis on source files.
Configures if needed, then runs clang-tidy via CMake target.

BUILD_TYPE:
  debug           Use debug build (default)
  relwithdebinfo  Use relwithdebinfo build

Options:
  -v, --verbose   Show verbose output
  -h, --help      Show this help
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -h|--help) usage ;;
        debug|relwithdebinfo) BUILD_TYPE="$1"; shift ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"

# Configure if needed (using CMake presets)
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "Build not configured. Running cmake --preset $BUILD_TYPE..."
    cmake --preset "$BUILD_TYPE"
fi

if $VERBOSE; then
    echo "Running clang-tidy for $BUILD_TYPE build..."
fi

# Copy compile_commands.json and run clang-tidy
cmake --build "$BUILD_DIR" --target copy-compile-commands run-clang-tidy
