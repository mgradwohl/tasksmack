#!/usr/bin/env bash
# Generate code coverage report using llvm-cov
# Usage: ./coverage.sh [OPTIONS]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Source common functions
# shellcheck source=tools/common.sh
source "$SCRIPT_DIR/common.sh"

# Validate prerequisites early
validate_coverage_prereqs || exit 1

VERBOSE=false
OPEN_REPORT=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build with coverage, run tests, and generate HTML coverage report.

Options:
  -v, --verbose      Show verbose output
  -o, --open         Open HTML report in browser after generation
  -h, --help         Show this help
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -o|--open) OPEN_REPORT=true; shift ;;
        -h|--help) usage ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/coverage"
COVERAGE_DIR="${PROJECT_ROOT}/coverage"

# Find llvm tools using common.sh functions
LLVM_PROFDATA="$(find_llvm_tool llvm-profdata)"
LLVM_COV="$(find_llvm_tool llvm-cov)"

if [[ -z "$LLVM_PROFDATA" ]]; then
    echo "Error: llvm-profdata not found. Install LLVM tools." >&2
    exit 1
fi

if [[ -z "$LLVM_COV" ]]; then
    echo "Error: llvm-cov not found. Install LLVM tools." >&2
    exit 1
fi

if $VERBOSE; then
    echo "Using llvm-profdata: $LLVM_PROFDATA"
    echo "Using llvm-cov: $LLVM_COV"
fi

# Step 1: Configure and build with coverage
echo "==> Configuring coverage build..."
PYTHON_EXE="$(find_python)" || {
    echo "Error: Python 3.14 or newer not found in PATH. Install Python 3.14 or newer." >&2
    exit 1
}
cmake --preset coverage -DPython3_EXECUTABLE="$PYTHON_EXE"

echo "==> Building..."
cmake --build --preset coverage

# Step 2: Run tests to generate profraw data
echo "==> Running tests..."
cd "$BUILD_DIR"
rm -f -- *.profraw default.profdata

# Set profraw output location
export LLVM_PROFILE_FILE="${BUILD_DIR}/coverage-%p.profraw"

# Set LD_LIBRARY_PATH so dlopen("libnvidia-ml.so.1") and dlopen("librocm_smi64.so.6")
# resolve to the mock shared libraries. CTest sets this automatically via the
# gtest_discover_tests ENVIRONMENT property, but this script runs the binary directly.
export LD_LIBRARY_PATH="${BUILD_DIR}/tests/mocks${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Run the test executable directly to capture coverage
./tests/TaskSmackTests

# Step 3: Merge profraw files into profdata
echo "==> Merging coverage data..."
$LLVM_PROFDATA merge -sparse "${BUILD_DIR}"/*.profraw -o "${BUILD_DIR}/default.profdata"

# Step 4: Generate HTML report
echo "==> Generating HTML report..."
mkdir -p "$COVERAGE_DIR"

$LLVM_COV show \
    "${BUILD_DIR}/tests/TaskSmackTests" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -format=html \
    -output-dir="$COVERAGE_DIR" \
    -show-line-counts-or-regions \
    -show-instantiations=false \
    -ignore-filename-regex='.*/(build|_deps|tests|\.cache)/.*'

# Step 5: Generate summary
echo "==> Coverage Summary:"
$LLVM_COV report \
    "${BUILD_DIR}/tests/TaskSmackTests" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -ignore-filename-regex='.*/(build|_deps|tests|\.cache)/.*'

echo ""
echo "HTML report generated at: ${COVERAGE_DIR}/index.html"

# Open in browser if requested
if $OPEN_REPORT; then
    if command -v xdg-open &> /dev/null; then
        xdg-open "${COVERAGE_DIR}/index.html"
    elif command -v open &> /dev/null; then
        open "${COVERAGE_DIR}/index.html"
    fi
fi
