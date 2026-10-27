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
PRESET="coverage"

usage() {
    local exit_code="${1:-0}"
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build with coverage, run tests, and generate HTML coverage report.

Options:
  -p, --preset NAME  CMake preset to use (default: coverage)
  -v, --verbose      Show verbose output
  -o, --open         Open HTML report in browser after generation
  -h, --help         Show this help
EOF
    exit "$exit_code"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--preset)
            if [[ $# -lt 2 ]]; then
                echo "Error: $1 requires a value" >&2
                usage 1
            fi
            PRESET="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=true; shift ;;
        -o|--open) OPEN_REPORT=true; shift ;;
        -h|--help) usage ;;
        *) echo "Error: Unknown argument: $1" >&2; usage 1 ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${PRESET}"
COVERAGE_DIR="${PROJECT_ROOT}/coverage"

# Find llvm tools using common.sh functions
LLVM_PROFDATA="$(find_llvm_tool llvm-profdata)"
LLVM_COV="$(find_llvm_tool llvm-cov)"
LLVM_OBJCOPY="$(find_llvm_tool llvm-objcopy)"

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
cmake --preset "$PRESET" -DPython3_EXECUTABLE="$PYTHON_EXE"

echo "==> Building..."
cmake --build --preset "$PRESET"

# Step 2: Run tests to generate profraw data
echo "==> Running tests..."
cd "$BUILD_DIR"
rm -f -- *.profraw default.profdata

# Set profraw output location
export LLVM_PROFILE_FILE="${BUILD_DIR}/coverage-%p.profraw"

# Set LD_LIBRARY_PATH so dlopen("libnvidia-ml.so.1") and dlopen("librocm_smi64.so.6")
# resolve to the mock shared libraries. CTest sets this automatically via the
# ENVIRONMENT_MODIFICATION test property (tests/CMakeLists.txt), but this script runs the binary directly.
export LD_LIBRARY_PATH="${BUILD_DIR}/tests/mocks${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Run the test executables directly to capture coverage. TaskSmackThemeTests links the real
# UI/Theme.cpp, which TaskSmackTests replaces with a stub (#1547); TaskSmackAppTests is built from the
# app's own objects, for the layers that need the whole app on a display (#1395). A failing suite still
# gets its coverage reported, so the failure can be diagnosed from the report, but the script exits
# non-zero at the end (#2251). Keep in sync with tools/coverage.ps1.
#
# The UI scenarios (#2711) run on a synthetic machine: tests/CMakeLists.txt gives UiScenarioTest
# TASKSMACK_SYNTHETIC, which the app reads once per process, so they get a run of their own here with
# the same value, as under CTest, and the rest of TaskSmackAppTests runs without it (#2927).
UI_SCENARIO_SYNTHETIC="$(sed -n 's/.*UiScenarioTest ENVIRONMENT "TASKSMACK_SYNTHETIC=\([^"]*\)".*/\1/p' "${PROJECT_ROOT}/tests/CMakeLists.txt")"
if [[ -z "$UI_SCENARIO_SYNTHETIC" ]]; then
    echo "Error: no UiScenarioTest TASKSMACK_SYNTHETIC in tests/CMakeLists.txt; keep this script in step with it." >&2
    exit 1
fi
FAILED_SUITES=()
# Each run: the suite, then its gtest filter and environment (either may be empty).
for run in "TaskSmackTests||" "TaskSmackThemeTests||" "TaskSmackAppTests|-UiScenarioTest.*|" \
    "TaskSmackAppTests|UiScenarioTest.*|TASKSMACK_SYNTHETIC=${UI_SCENARIO_SYNTHETIC}"; do
    IFS='|' read -r suite filter run_env <<<"$run"
    command=(env)
    [[ -n "$run_env" ]] && command+=("$run_env")
    command+=("./tests/${suite}")
    [[ -n "$filter" ]] && command+=("--gtest_filter=${filter}")
    suite_status=0
    "${command[@]}" || suite_status=$?
    if [[ $suite_status -ne 0 ]]; then
        echo "Warning: ${suite}${filter:+ (${filter})} failed (exit code ${suite_status}); generating the coverage report, then failing." >&2
        FAILED_SUITES+=("${suite}${filter:+ (${filter})}")
    fi
done

# The application itself, once (#2965): the tests never enter main.cpp's runApp(), so its startup, layer
# composition and teardown went uncounted. A fresh config directory, the synthetic machine and a screenshot
# request that closes it as Close would (TASKSMACK_SCREENSHOT, as the release smoke checks use): it must
# exit 0, write a PNG and save its config. Its profile lands beside the tests' (LLVM_PROFILE_FILE's %p).
APP_SMOKE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tasksmack-coverage-app.XXXXXX")"
app_status=0
env TASKSMACK_CONFIG_DIR="${APP_SMOKE_DIR}/config" TASKSMACK_SYNTHETIC="${UI_SCENARIO_SYNTHETIC}" \
    TASKSMACK_SCREENSHOT="${APP_SMOKE_DIR}/frame.png;after=5" \
    timeout 120 "${BUILD_DIR}/bin/TaskSmack" >"${APP_SMOKE_DIR}/app.log" 2>&1 || app_status=$?
app_problem=""
if [[ $app_status -ne 0 ]]; then
    app_problem="exit code ${app_status}"
elif [[ "$(head -c 8 "${APP_SMOKE_DIR}/frame.png" 2>/dev/null | od -An -tx1 | tr -d ' \n')" != "89504e470d0a1a0a" ]]; then
    app_problem="no PNG at ${APP_SMOKE_DIR}/frame.png"
elif ! grep -q '^config_version = ' "${APP_SMOKE_DIR}/config/config.toml" 2>/dev/null; then
    app_problem="no saved config.toml"
fi
if [[ -n "$app_problem" ]]; then
    echo "Warning: the TaskSmack smoke run failed (${app_problem}); its log:" >&2
    tail -n 40 "${APP_SMOKE_DIR}/app.log" >&2 || true
    FAILED_SUITES+=("TaskSmack (the application)")
fi
rm -rf "${APP_SMOKE_DIR}"

# Step 3: Merge profraw files into profdata
echo "==> Merging coverage data..."
$LLVM_PROFDATA merge -sparse "${BUILD_DIR}"/*.profraw -o "${BUILD_DIR}/default.profdata"

# Files excluded from coverage: generated/third-party paths and test sources only (benchmarks/ holds
# test-support headers the tests include, #1395). ImGui panel code
# (Widgets.h, the panel headers) is deliberately NOT excluded any more (#1131): untested code has
# to count against the total. tools/coverage.ps1 also drops LCOV_EXCL-marked lines, which mark only
# Windows-only code (#2039); this script honours no markers, so every Linux line counts.
COV_IGNORE_REGEX='.*/(build|_deps|tests|benchmarks|\.cache)/.*'

# llvm-cov only reports files compiled into the binaries it is given, so with TaskSmackTests
# alone the ~27% of src/ that is never linked into the tests (panels, layers, Theme.cpp,
# main.cpp, ...) silently drops out of the denominator (#1131). The coverage preset
# instruments every target, so also pass the app binary as an extra -object: its coverage
# mapping covers all of src/, and every file the tests never execute is reported at 0%.
APP_BINARY="${BUILD_DIR}/bin/TaskSmack"
if [[ ! -x "$APP_BINARY" ]]; then
    echo "Error: ${APP_BINARY} not found; without it the report would only count files linked into TaskSmackTests." >&2
    exit 1
fi
COV_OBJECTS=("${BUILD_DIR}/tests/TaskSmackTests" -object "${BUILD_DIR}/tests/TaskSmackThemeTests" -object "${BUILD_DIR}/tests/TaskSmackAppTests" -object "$APP_BINARY")

# Step 4: Generate HTML report
echo "==> Generating HTML report..."
mkdir -p "$COVERAGE_DIR"

$LLVM_COV show \
    "${COV_OBJECTS[@]}" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -format=html \
    -output-dir="$COVERAGE_DIR" \
    -show-line-counts-or-regions \
    -show-instantiations=false \
    -ignore-filename-regex="${COV_IGNORE_REGEX}"

# Step 5: Generate LCOV file for Codecov
echo "==> Generating LCOV report..."
$LLVM_COV export \
    "${COV_OBJECTS[@]}" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -format=lcov \
    -ignore-filename-regex="${COV_IGNORE_REGEX}" \
    > "${COVERAGE_DIR}/coverage.lcov"

# Step 6: Generate summary
echo "==> Coverage Summary:"
$LLVM_COV report \
    "${COV_OBJECTS[@]}" \
    -instr-profile="${BUILD_DIR}/default.profdata" \
    -ignore-filename-regex="${COV_IGNORE_REGEX}"

# llvm-cov warns above that a couple of hundred functions "have mismatched data". They are clang's
# unused-function placeholders, which carry no counts, so nothing is lost; this confirms that every
# mismatch is one, and warns (without failing the run) if a real record ever mismatches (#1544).
echo ""
echo "==> Checking mismatched functions..."
if [[ -z "$LLVM_OBJCOPY" ]]; then
    echo "Warning: llvm-objcopy not found; skipping the mismatched-functions check." >&2
elif ! "$PYTHON_EXE" -I -X utf8 "${SCRIPT_DIR}/coverage-mismatches.py" \
    --profdata "${BUILD_DIR}/default.profdata" \
    --llvm-profdata "$LLVM_PROFDATA" \
    --llvm-objcopy "$LLVM_OBJCOPY" \
    "${BUILD_DIR}/tests/TaskSmackTests" "${BUILD_DIR}/tests/TaskSmackThemeTests" "${BUILD_DIR}/tests/TaskSmackAppTests" "$APP_BINARY"; then
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        echo "::warning title=Coverage::Some functions' coverage is missing from the report: a real record mismatched (see tools/coverage-mismatches.py, #1544)"
    fi
fi

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

if [[ ${#FAILED_SUITES[@]} -gt 0 ]]; then
    echo "Error: test failures in: ${FAILED_SUITES[*]}. The coverage report was still generated." >&2
    exit 1
fi
