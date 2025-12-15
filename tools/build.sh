#!/usr/bin/env bash
# DEPRECATED: Use CMake presets instead:
#   cmake --build --preset debug
#   cmake --build --preset release
# See CMakePresets.json for all available presets.
#
# This script is kept for backwards compatibility.
# Usage: ./build.sh [OPTIONS] [BUILD_TYPE]
set -euo pipefail

echo "NOTE: This script is deprecated. Consider using CMake presets instead:" >&2
echo "  cmake --build --preset debug" >&2
echo "  cmake --build --preset release" >&2
echo "" >&2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Defaults
BUILD_TYPE="debug"
VERBOSE=false
TARGET=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE]

Build project. Run configure.sh first if the build directory doesn't exist.

BUILD_TYPE:
  debug           Debug build (default)
  relwithdebinfo  Release with debug info
  release         Release build
  optimized       Optimized build

Options:
  -v, --verbose       Show verbose build output
  -t, --target NAME   Build specific target(s)
  -h, --help          Show this help

Examples:
  $(basename "$0")                    # Build debug
  $(basename "$0") release            # Build release
  $(basename "$0") -v optimized       # Verbose optimized build
  $(basename "$0") -t run-clang-tidy  # Run clang-tidy target
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -t|--target) TARGET="$2"; shift 2 ;;
        -h|--help) usage ;;
        debug|relwithdebinfo|release|optimized)
            BUILD_TYPE="$1"; shift ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"

# Check if configured
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "Error: Build directory '$BUILD_DIR' not configured." >&2
    echo "Run 'tools/configure.sh $BUILD_TYPE' first." >&2
    exit 1
fi

# Build arguments
CMAKE_ARGS=(--build "$BUILD_DIR")

if [[ -n "$TARGET" ]]; then
    CMAKE_ARGS+=(--target "$TARGET")
fi

if $VERBOSE; then
    CMAKE_ARGS+=(--verbose)
    echo "Building $BUILD_TYPE in $BUILD_DIR"
    [[ -n "$TARGET" ]] && echo "Target: $TARGET"
fi

cmake "${CMAKE_ARGS[@]}"
