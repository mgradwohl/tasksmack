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

Environment:
  TASKSMACK_COVERAGE_ROOT=1  Also run the root tests (DetachedSpawnTest.AsRoot*, UdpByteCounterRootTest) as root
                             with sudo -n, so their paths are counted (CI's coverage job sets it; needs passwordless sudo)
  TASKSMACK_COVERAGE_REAL_SYSTEM=1
                             And let that root pass run the CI real-system tests among them (the eBPF
                             UDP counter, #3374): it passes
                             TASKSMACK_CI_REAL_SYSTEM=1 to them, which their guard honours only on a
                             disposable GitHub-hosted runner. CI's coverage job only; never set it locally
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
rm -rf -- *.profraw default.profdata coverage-root

# Set profraw output location. Continuous mode (%c, #3221): each process maps its profile at startup and
# writes its counters through the mapping, so a forked child that ends in _exit() or execve() -- DetachedSpawn's
# children, before the program they start -- still has its counts kept, in its parent's file (a fork shares the
# mapping). Without it those children's lines read as never run, though the tests run them. The coverage build
# has the runtime counter relocation continuous mode needs on Linux (cmake/CompilerOptions.cmake, #3160).
export LLVM_PROFILE_FILE="${BUILD_DIR}/coverage-%c%p.profraw"

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

# A RunAs's supplementary groups (#3298) and the eBPF UDP counter (#3366, #3580), as root (#3058, #3108):
# DetachedSpawnTest.AsRoot* and UdpByteCounterRootTest skip unless they run as root, so with
# TASKSMACK_COVERAGE_ROOT=1 (CI's coverage job, where sudo needs no password) they run once more under
# sudo. Their children end with _exit() or drop root first, so this run uses continuous mode (%c): each
# process's profile is opened and mapped at startup, and its counters are written through the mapping. Its profiles go to a world-writable folder of their own; the merge
# below takes them with the rest. A failure, or a skip for any other reason than the one below, counts as a
# failed suite, and so does any test the filter selects that neither passed nor skipped that way.
if [[ "${TASKSMACK_COVERAGE_ROOT:-0}" == 1 ]]; then
    ROOT_PROFILE_DIR="${BUILD_DIR}/coverage-root"
    ROOT_TEST_FILTER='DetachedSpawnTest.AsRoot*:*/UdpByteCounterRootTest.*'
    # The CI real-system tests' guard (tests/Support/CiRealSystem.h) reads these three; sudo drops them, so they're
    # passed by name. TASKSMACK_CI_REAL_SYSTEM is 1 only when CI's coverage job asks for it (#3374), so anywhere else
    # those tests skip as guarded, which the counts below accept.
    ROOT_REAL_SYSTEM=0
    [[ "${TASKSMACK_COVERAGE_REAL_SYSTEM:-0}" == 1 ]] && ROOT_REAL_SYSTEM=1
    # Every test the filter selects ("  TestName" lines under each "Suite." line), counted before the run.
    root_listed="$(./tests/TaskSmackTests --gtest_filter="${ROOT_TEST_FILTER}" --gtest_list_tests | grep -c '^  ' || true)"
    mkdir -p "$ROOT_PROFILE_DIR" && chmod 1777 "$ROOT_PROFILE_DIR"
    root_log="$(mktemp "${TMPDIR:-/tmp}/tasksmack-coverage-root.XXXXXX")"
    root_status=0
    # shellcheck disable=SC2024 # the log is meant to be the invoking user's file, not root's
    sudo -n env LLVM_PROFILE_FILE="${ROOT_PROFILE_DIR}/coverage-%c%p.profraw" TASKSMACK_CI_REAL_SYSTEM="${ROOT_REAL_SYSTEM}" \
        GITHUB_ACTIONS="${GITHUB_ACTIONS:-}" RUNNER_ENVIRONMENT="${RUNNER_ENVIRONMENT:-}" ./tests/TaskSmackTests \
        --gtest_filter="${ROOT_TEST_FILTER}" >"$root_log" 2>&1 || root_status=$?
    # The CI real-system tests among them (#3491) skip here, with ciRealSystemSkipReason()'s message
    # (tests/Support/CiRealSystem.h): they need TASKSMACK_CI_REAL_SYSTEM on a disposable runner, which CI's PR root
    # step has, and this job only when TASKSMACK_COVERAGE_REAL_SYSTEM asks for it. Without it those skips are expected
    # (#3557), so a new such test needs no exclusion here (#3524 excluded two by name). With it they aren't: a guard
    # that still skips (a runner variable missing) would publish coverage without the paths it was asked to count.
    # Any other skip never is, and every listed test must pass or skip as expected.
    read -r root_guarded root_unexpected < <(awk '
        /^\[ RUN      \] / { test = $4; guarded = 0; next }
        /^a CI real-system test: / { guarded = 1 }
        /^\[  SKIPPED \] / && test != "" && $4 == test { if (guarded) g++; else u++; test = "" }
        END { print g + 0, u + 0 }' "$root_log")
    root_passed="$(sed -n 's/^\[  PASSED  \] \([0-9][0-9]*\) tests*\..*/\1/p' "$root_log" | tail -n 1)"
    if [[ $root_status -ne 0 || $root_unexpected -ne 0 || ($ROOT_REAL_SYSTEM -eq 1 && $root_guarded -ne 0) ||
        -z "$root_passed" || $root_passed -eq 0 ||
        $((root_passed + root_guarded)) -ne $root_listed ]]; then
        echo "Warning: the root tests (DetachedSpawnTest.AsRoot*, UdpByteCounterRootTest) didn't run and pass as root" \
            "(exit code ${root_status}; ${root_listed} listed, ${root_passed:-0} passed, ${root_guarded} CI real-system skips," \
            "${root_unexpected} other skips); its log:" >&2
        tail -n 30 "$root_log" >&2 || true
        FAILED_SUITES+=("TaskSmackTests (the root tests, as root)")
    fi
    rm -f "$root_log"
fi

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
shopt -s nullglob
profiles=("${BUILD_DIR}"/*.profraw "${BUILD_DIR}"/coverage-root/*.profraw)
shopt -u nullglob
$LLVM_PROFDATA merge -sparse "${profiles[@]}" -o "${BUILD_DIR}/default.profdata"

# Files excluded from coverage: generated/third-party paths and test sources only (benchmarks/ holds
# test-support headers the tests include, #1395). ImGui panel code
# (Widgets.h, the panel headers) is deliberately NOT excluded any more (#1131): untested code has
# to count against the total. The LCOV report Codecov gets leaves out the lines no test can run: those
# marked LCOV_EXCL_START/STOP or LCOV_EXCL_LINE, each with its reason (#2039, #3221), and defaulted
# comparison operators, which clang never covers fully. tools/lcov-exclude.py drops them here and
# tools/coverage.ps1 does the same on Windows; the HTML report and the summary below still count them.
# build, tests and benchmarks are anchored at the checkout, so one under a folder of that name still
# reports its files (#3528). Keep in sync with tools/coverage.ps1 $IgnoreRegex.
COV_IGNORE_REGEX="$(coverage_ignore_regex "$PROJECT_ROOT")"

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
# The ignore regex is anchored at the checkout (#3528), so a root CMake spelled some other way would
# leave tests/ in the report, and a match on the checkout's own path would leave src/ out. Either is a
# wrong report, never a real result, so it fails here.
PROJECT_ROOT_PHYSICAL="$(cd "$PROJECT_ROOT" && pwd -P)"
src_count=0
leaked=""
while IFS= read -r reported; do
    reported="${reported#SF:}"
    for root in "$PROJECT_ROOT" "$PROJECT_ROOT_PHYSICAL"; do
        if [[ "$reported" == "$root"/* ]]; then
            case "${reported#"$root"/}" in
                src/*) src_count=$((src_count + 1)) ;;
                build/* | tests/* | benchmarks/*) leaked="${leaked:-$reported}" ;;
            esac
            break
        fi
    done
done < <(grep '^SF:' "${COVERAGE_DIR}/coverage.lcov" || true)
if [[ "$src_count" -eq 0 || -n "$leaked" ]]; then
    echo "Error: the LCOV report has ${src_count} file(s) under src/, or one -ignore-filename-regex should have left out (${leaked:-none}); regex: ${COV_IGNORE_REGEX}" >&2
    exit 1
fi
dropped_lines="$("$PYTHON_EXE" -I -X utf8 "${SCRIPT_DIR}/lcov-exclude.py" "${COVERAGE_DIR}/coverage.lcov")"
echo "Left ${dropped_lines} structurally untestable line(s) out of the LCOV report (#2039, #3221)."

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
