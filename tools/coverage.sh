#!/usr/bin/env bash
# Generate code coverage report using llvm-cov
# Usage: ./coverage.sh [OPTIONS]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

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

# Find llvm tools
LLVM_PROFDATA=""
LLVM_COV=""
for ver in 22 21 20 19 18 17; do
    if [[ -x "/usr/lib/llvm-$ver/bin/llvm-profdata" ]]; then
        LLVM_PROFDATA="/usr/lib/llvm-$ver/bin/llvm-profdata"
        LLVM_COV="/usr/lib/llvm-$ver/bin/llvm-cov"
        break
    fi
done

if [[ -z "$LLVM_PROFDATA" ]]; then
    if command -v llvm-profdata &> /dev/null; then
        LLVM_PROFDATA="llvm-profdata"
        LLVM_COV="llvm-cov"
    else
        echo "Error: llvm-profdata not found. Install LLVM tools." >&2
        exit 1
    fi
fi

if $VERBOSE; then
    echo "Using llvm-profdata: $LLVM_PROFDATA"
    echo "Using llvm-cov: $LLVM_COV"
fi

# Step 1: Configure and build with coverage
echo "==> Configuring coverage build..."
cmake --preset coverage

echo "==> Building..."
cmake --build --preset coverage

# Step 2: Run tests to generate profraw data
echo "==> Running tests..."
cd "$BUILD_DIR"
rm -f *.profraw default.profdata

# Set profraw output location
export LLVM_PROFILE_FILE="${BUILD_DIR}/coverage-%p.profraw"

# Run the test executable directly to capture coverage
./tests/MyProject_tests

# Step 3: Merge profraw files into profdata
echo "==> Merging coverage data..."
$LLVM_PROFDATA merge -sparse "${BUILD_DIR}"/*.profraw -o "${BUILD_DIR}/default.profdata"

# Step 4: Generate HTML report
echo "==> Generating HTML report..."
mkdir -p "$COVERAGE_DIR"

$LLVM_COV show \
    "${BUILD_DIR}/tests/MyProject_tests" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -format=html \
    -output-dir="$COVERAGE_DIR" \
    -show-line-counts-or-regions \
    -show-instantiations=false \
    -ignore-filename-regex='.*/(build|_deps|tests)/.*'

# Step 5: Generate summary
echo "==> Coverage Summary:"
$LLVM_COV report \
    "${BUILD_DIR}/tests/MyProject_tests" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -ignore-filename-regex='.*/(build|_deps|tests)/.*'

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
