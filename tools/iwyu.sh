#!/usr/bin/env bash
# Run include-what-you-use (IWYU) on source files
# Uses compile_commands.json from the build directory
# Usage: ./iwyu.sh [OPTIONS] [BUILD_TYPE] [FILES...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Defaults
BUILD_TYPE="debug"
VERBOSE=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
FILES=()
FIX=false
REPORT_ONLY=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE] [FILES...]

Run include-what-you-use (IWYU) analysis on source files.
IWYU analyzes #include directives and suggests additions/removals.

Prerequisites:
  - IWYU must be installed (apt install iwyu or brew install include-what-you-use)
  - Project must be configured and built at least once:
    cmake --preset debug && cmake --build build/debug

BUILD_TYPE:
  debug           Use debug build (default)
  relwithdebinfo  Use relwithdebinfo build

Options:
  -v, --verbose     Show verbose output with per-file progress
  -j, --jobs N      Number of parallel jobs (default: $JOBS)
  -f, --fix         Apply suggested fixes using iwyu-fix-includes
  -r, --report      Generate report only, don't fail on issues
  -h, --help        Show this help

FILES:
  Optional list of specific files to analyze (default: all src/**/*.cpp recursively)

Examples:
  $(basename "$0")                          # Analyze all files
  $(basename "$0") -v -j 8                  # Verbose with 8 jobs
  $(basename "$0") --fix                    # Apply suggested fixes
  $(basename "$0") src/Domain/ProcessModel.cpp  # Analyze specific file

Note: IWYU suggestions are advisory. Review before applying fixes.
      Some suggestions may not be appropriate for this codebase.
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -j|--jobs)
            if [[ $# -lt 2 ]]; then
                echo "Error: --jobs requires a positive integer argument" >&2
                exit 1
            fi
            if ! [[ "$2" =~ ^[1-9][0-9]*$ ]]; then
                echo "Error: Invalid jobs value '$2'. --jobs must be a positive integer" >&2
                exit 1
            fi
            JOBS="$2"
            shift 2
            ;;
        -f|--fix) FIX=true; shift ;;
        -r|--report) REPORT_ONLY=true; shift ;;
        -h|--help) usage ;;
        debug|relwithdebinfo) BUILD_TYPE="$1"; shift ;;
        *.cpp|*.h) FILES+=("$1"); shift ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"
COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"

# Find iwyu_tool.py (the wrapper script that processes compile_commands.json)
IWYU_TOOL=""
for candidate in \
    "iwyu_tool.py" \
    "iwyu_tool" \
    "/usr/bin/iwyu_tool.py" \
    "/usr/bin/iwyu_tool" \
    "/usr/local/bin/iwyu_tool.py" \
    "/usr/local/bin/iwyu_tool"; do
    if command -v "$candidate" &>/dev/null; then
        IWYU_TOOL="$candidate"
        break
    fi
done

# If iwyu_tool not found, check for include-what-you-use directly
IWYU=""
if [[ -z "$IWYU_TOOL" ]]; then
    for candidate in \
        "include-what-you-use" \
        "iwyu" \
        "/usr/bin/include-what-you-use" \
        "/usr/local/bin/include-what-you-use"; do
        if command -v "$candidate" &>/dev/null; then
            IWYU="$candidate"
            break
        fi
    done
fi

if [[ -z "$IWYU_TOOL" ]] && [[ -z "$IWYU" ]]; then
    echo "Error: include-what-you-use not found in PATH" >&2
    echo "" >&2
    echo "Install IWYU:" >&2
    echo "  Ubuntu/Debian: sudo apt install iwyu" >&2
    echo "  macOS:         brew install include-what-you-use" >&2
    echo "  From source:   https://github.com/include-what-you-use/include-what-you-use" >&2
    exit 1
fi

# Version check: warn if IWYU and Clang versions are mismatched
check_version_compatibility() {
    local iwyu_clang_version clang_version
    iwyu_clang_version=$(include-what-you-use --version 2>&1 | awk '/clang version/ { print $3; exit }' | cut -d. -f1 || echo "")
    clang_version=$(clang --version 2>&1 | awk '/clang version/ { print $3; exit }' | cut -d. -f1 || echo "")

    if [[ -n "$iwyu_clang_version" ]] && [[ -n "$clang_version" ]]; then
        if [[ "$iwyu_clang_version" != "$clang_version" ]]; then
            echo "Warning: IWYU (clang $iwyu_clang_version) and project clang ($clang_version) version mismatch" >&2
            echo "  This may cause false positives or assertion failures." >&2
            echo "  Consider building IWYU from source against clang $clang_version," >&2
            echo "  or rely on CI results where versions are more aligned." >&2
            echo "" >&2
        fi
    fi
}

if $VERBOSE; then
    check_version_compatibility
    if [[ -n "$IWYU_TOOL" ]]; then
        echo "Using iwyu_tool: $IWYU_TOOL"
    else
        echo "Using include-what-you-use: $IWYU"
    fi
fi

# Check for build prerequisites
if [[ ! -d "$BUILD_DIR" ]] || [[ ! -f "$COMPILE_COMMANDS" ]]; then
    echo "Error: Build directory or compile_commands.json not found" >&2
    echo "" >&2
    echo "IWYU requires the project to be configured and built at least once." >&2
    echo "Please run the following commands first:" >&2
    echo "" >&2
    echo "  cmake --preset $BUILD_TYPE" >&2
    echo "  cmake --build build/$BUILD_TYPE" >&2
    echo "" >&2
    echo "For more information, see CONTRIBUTING.md" >&2
    exit 1
fi

# Verify build.ninja exists (should exist after configuration)
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "Error: Build system not configured properly" >&2
    echo "Expected to find: $BUILD_DIR/build.ninja" >&2
    echo "" >&2
    echo "Please reconfigure the build:" >&2
    echo "  cmake --preset $BUILD_TYPE" >&2
    exit 1
fi

# Strip PCH flags from compile_commands.json (IWYU doesn't support PCH)
if $VERBOSE; then
    echo "Stripping PCH flags from compile_commands.json..."
fi
# Use portable sed in-place editing that works on both GNU sed (Linux) and BSD sed (macOS)
if [[ "$(uname)" == "Darwin" ]]; then
    # BSD sed requires a separate argument for backup extension
    sed -i '' \
        -e 's/-Xclang -include-pch -Xclang [^ ]*\.pch//g' \
        -e 's/-Xclang -emit-pch//g' \
        -e 's/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g' \
        -e 's/-Winvalid-pch//g' \
        -e 's/-fpch-instantiate-templates//g' \
        -e 's/@[^ ]*\.modmap//g' \
        -e 's/-fmodule-output=[^ ]*//g' \
        "$COMPILE_COMMANDS"
else
    # GNU sed: -i with no argument for in-place without backup
    sed -i \
        -e 's/-Xclang -include-pch -Xclang [^ ]*\.pch//g' \
        -e 's/-Xclang -emit-pch//g' \
        -e 's/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g' \
        -e 's/-Winvalid-pch//g' \
        -e 's/-fpch-instantiate-templates//g' \
        -e 's/@[^ ]*\.modmap//g' \
        -e 's/-fmodule-output=[^ ]*//g' \
        "$COMPILE_COMMANDS"
fi

# Verify IWYU mapping file exists
IWYU_MAPPING="${PROJECT_ROOT}/.iwyu.imp"
if [[ ! -f "$IWYU_MAPPING" ]]; then
    echo "Error: IWYU mapping file not found: $IWYU_MAPPING" >&2
    echo "The .iwyu.imp file is required in the repository root." >&2
    exit 1
fi

# Detect current platform to exclude non-current platform files by default
CURRENT_PLATFORM="unknown"
UNAME_OUT=$(uname -s 2>/dev/null || echo "")
case "$UNAME_OUT" in
    Linux*) CURRENT_PLATFORM="linux" ;;
    Darwin*) CURRENT_PLATFORM="linux" ;; # macOS builds target Linux probes
    CYGWIN*|MINGW*|MSYS*|Windows_NT) CURRENT_PLATFORM="windows" ;;
esac

# Determine files to analyze
if [[ ${#FILES[@]} -eq 0 ]]; then
    # Get all source files from project, excluding other-platform files
    if [[ "$CURRENT_PLATFORM" == "windows" ]]; then
        mapfile -t SOURCE_FILES < <(find "${PROJECT_ROOT}/src" \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -type f \
            ! -path "*/Platform/Linux/*" 2>/dev/null)
    else
        mapfile -t SOURCE_FILES < <(find "${PROJECT_ROOT}/src" \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -type f \
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
    echo "No source files found to analyze."
    exit 0
fi

if $VERBOSE; then
    echo "Running IWYU on ${#SOURCE_FILES[@]} files with $JOBS parallel jobs..."
    echo ""
fi

# Create temporary file for output
IWYU_OUTPUT=$(mktemp)
trap 'rm -f "$IWYU_OUTPUT"' EXIT

# Run IWYU
HAS_ISSUES=0

if [[ -n "$IWYU_TOOL" ]]; then
    # Use iwyu_tool.py for better integration with compile_commands.json
    # IWYU-specific args go after -- separator
    IWYU_TOOL_ARGS=(
        -p "$BUILD_DIR"
        -j "$JOBS"
    )
    IWYU_ARGS=(
        "-Xiwyu" "--mapping_file=$IWYU_MAPPING"
        "-Xiwyu" "--cxx17ns"
    )

    if [[ ${#FILES[@]} -gt 0 ]]; then
        # Analyze specific files
        for file in "${SOURCE_FILES[@]}"; do
            if $VERBOSE; then
                echo "  Analyzing: ${file#"${PROJECT_ROOT}"/}"
            fi
            "$IWYU_TOOL" "${IWYU_TOOL_ARGS[@]}" "$file" -- "${IWYU_ARGS[@]}" 2>&1 | tee -a "$IWYU_OUTPUT" || true
        done
    else
        # Analyze all files
        "$IWYU_TOOL" "${IWYU_TOOL_ARGS[@]}" -- "${IWYU_ARGS[@]}" 2>&1 | tee "$IWYU_OUTPUT" || true
    fi
else
    # Fall back to running include-what-you-use directly
    # Extract compiler flags from compile_commands.json for accurate analysis
    for file in "${SOURCE_FILES[@]}"; do
        if $VERBOSE; then
            echo "  Analyzing: ${file#"${PROJECT_ROOT}"/}"
        fi

        # Extract compile flags for this file from compile_commands.json
        # This ensures IWYU has access to include paths, defines, and other compilation flags
        COMPILE_FLAGS=$(python3 -c "
import json
import sys
import os
import shlex

compile_commands_path = sys.argv[1]
target_file = sys.argv[2]

with open(compile_commands_path) as f:
    commands = json.load(f)

# Normalize target file path for comparison
target_file = os.path.abspath(target_file)

# Find the compile command for this file
for cmd in commands:
    file_path = cmd.get('file', '')
    # Normalize and compare full paths
    if os.path.abspath(file_path) == target_file:
        command = cmd.get('command', '')
        # Extract flags: remove compiler name and output-related flags
        # Keep: -I, -D, -std, -f flags (except -fpch*), -W flags, --sysroot, etc.
        tokens = shlex.split(command)
        flags = []
        skip_next = False
        cpp_extensions = ('.cpp', '.cc', '.cxx', '.c++', '.C', '.CPP')
        for i, token in enumerate(tokens):
            if skip_next:
                skip_next = False
                continue
            # Skip compiler name (first token)
            if i == 0:
                continue
            # Skip output flags
            if token in ['-o', '-c', '-MF', '-MT', '-MD']:
                skip_next = True
                continue
            # Skip output files (token.endswith('.o'))
            if token.startswith('-o') or token.endswith('.o'):
                continue
            # Skip source files - must look like a path (contains / or is just filename)
            if ('/' in token or not token.startswith('-')) and token.endswith(cpp_extensions):
                continue
            # Keep relevant flags
            if token.startswith('-I') or token.startswith('-D') or \
               token.startswith('-std') or token.startswith('--sysroot') or \
               (token.startswith('-f') and not token.startswith('-fpch')) or \
               (token.startswith('-W') and not token.startswith('-Winvalid-pch')):
                flags.append(token)
        print(' '.join(flags))
        break
" "$COMPILE_COMMANDS" "${file}" 2>/dev/null || echo "")

        # Direct IWYU invocation with extracted compile flags
        if [[ -n "$COMPILE_FLAGS" ]]; then
            $IWYU \
                $COMPILE_FLAGS \
                -Xiwyu --mapping_file="$IWYU_MAPPING" \
                -Xiwyu --cxx17ns \
                "$file" 2>&1 | tee -a "$IWYU_OUTPUT" || true
        else
            # Fallback: run without extracted flags (may produce less accurate results)
            if $VERBOSE; then
                echo "  Warning: Could not extract compile flags for ${file#"${PROJECT_ROOT}"/}"
            fi
            $IWYU \
                -Xiwyu --mapping_file="$IWYU_MAPPING" \
                -Xiwyu --cxx17ns \
                "$file" 2>&1 | tee -a "$IWYU_OUTPUT" || true
        fi
    done
fi

# Check for issues in output
if grep -q "should add these lines:" "$IWYU_OUTPUT" || \
   grep -q "should remove these lines:" "$IWYU_OUTPUT"; then
    HAS_ISSUES=1
fi

# Apply fixes if requested
if $FIX && [[ $HAS_ISSUES -ne 0 ]]; then
    echo ""
    echo "Applying IWYU suggestions..."

    # Find fix_includes.py
    FIX_INCLUDES=""
    for candidate in \
        "fix_includes.py" \
        "iwyu-fix-includes" \
        "/usr/bin/fix_includes.py" \
        "/usr/bin/iwyu-fix-includes"; do
        if command -v "$candidate" &>/dev/null; then
            FIX_INCLUDES="$candidate"
            break
        fi
    done

    if [[ -n "$FIX_INCLUDES" ]]; then
        "$FIX_INCLUDES" < "$IWYU_OUTPUT"
        echo "Fixes applied. Please review the changes."
    else
        echo "Warning: fix_includes.py not found. Manual fixes required."
    fi
fi

# Summary
echo ""
if [[ $HAS_ISSUES -ne 0 ]]; then
    echo "IWYU found include issues. See suggestions above."
    if ! $FIX; then
        echo "Run with --fix to apply suggested changes."
    fi
    if $REPORT_ONLY; then
        echo "(Report-only mode: not failing)"
        exit 0
    fi
    exit 1
else
    echo "IWYU completed successfully - no issues found."
    exit 0
fi
