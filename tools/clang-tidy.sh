#!/usr/bin/env bash
# Run clang-tidy on source files with parallel execution
# Uses .clang-tidy configuration from project root
# Usage: ./clang-tidy.sh [OPTIONS] [BUILD_TYPE | PRESET | --build-dir DIR] [FILES...]
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
BUILD_NAME=""    # debug, relwithdebinfo or another configure preset, as given
BUILD_DIR_ARG="" # --build-dir DIR, as given
VERBOSE=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CHANGED_ONLY=false
CACHE_DIR=""
FILES=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD_TYPE | PRESET | --build-dir DIR] [FILES...]

Run clang-tidy static analysis on source files with parallel execution.
Configures if needed, strips module and PCH flags, and runs clang-tidy on all source files.

BUILD_TYPE:
  debug           Use build/debug, configuring it if needed (default)
  relwithdebinfo  Use build/relwithdebinfo, configuring it if needed

PRESET:
  Any other configure preset in CMakePresets.json (tsan, asan-ubsan, ...): use the
  compile_commands.json in the preset's binaryDir. Never configures: run cmake --preset
  PRESET first. The flags are stripped in a temporary copy; the tree is left untouched.

Options:
      --build-dir DIR Use the compile_commands.json in DIR, any configured tree. Like PRESET,
                    never configures and strips the flags in a temporary copy
  -v, --verbose     Show verbose output with per-file progress
  -j, --jobs N      Number of parallel jobs (default: $JOBS)
  -c, --changed-only Only analyze C++ files changed on this branch (vs. its merge-base with main, plus untracked)
      --cache-dir DIR Skip files that passed before with identical inputs, and record clean results
                    in DIR (tools/tidy-cache.py; needs python3 and clang-scan-deps)
  -h, --help        Show this help

FILES:
  Optional list of specific files to analyze (default: all src/*.cpp)

Examples:
  $(basename "$0")                          # Analyze all files
  $(basename "$0") -v -j 8                  # Verbose with 8 jobs
  $(basename "$0") --changed-only           # Only analyze changed files
  $(basename "$0") tsan --changed-only      # Changed files, with build/tsan's flags
  $(basename "$0") --build-dir build/asan-ubsan src/App/main.cpp  # One file, that tree's flags
  $(basename "$0") src/Domain/ProcessModel.cpp  # Analyze specific file
EOF
    exit "${1:-0}"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -j|--jobs)
            if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]*$ ]]; then
                echo "Error: $1 requires a positive integer" >&2
                exit 1
            fi
            JOBS="$2"; shift 2 ;;
        -c|--changed-only) CHANGED_ONLY=true; shift ;;
        --cache-dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: --cache-dir requires a directory" >&2
                exit 1
            fi
            CACHE_DIR="$2"; shift 2 ;;
        --build-dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: --build-dir requires a directory" >&2
                exit 1
            fi
            BUILD_DIR_ARG="$2"; shift 2 ;;
        -h|--help) usage ;;
        *.cpp|*.h) FILES+=("$1"); shift ;;
        -*) echo "Error: Unknown argument: $1" >&2; usage 1 ;;
        # debug, relwithdebinfo or another configure preset, checked once every argument is parsed
        *) BUILD_NAME="$1"; shift ;;
    esac
done

PYTHON_EXE=$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo "")

# Print the binaryDir of configure preset $1, from CMakePresets.json and CMakeUserPresets.json (and
# their includes): its own or the first one its inherits chain gives, with the macros expanded.
# Fails, printing nothing, for a name that isn't a visible configure preset with a binaryDir.
resolve_preset_binary_dir() {
    "$PYTHON_EXE" -I -X utf8 - "$PROJECT_ROOT" "$1" <<'PY'
import json, os, re, sys

root, name = sys.argv[1], sys.argv[2]
presets = {}


def load(path, seen):
    path = os.path.normpath(path)
    if path in seen or not os.path.isfile(path):
        return
    seen.add(path)
    with open(path, encoding="utf-8-sig") as f:
        data = json.load(f)
    for include in data.get("include", []):
        load(os.path.join(os.path.dirname(path), include), seen)
    for preset in data.get("configurePresets", []):
        presets.setdefault(preset["name"], preset)


def binary_dir(preset_name, depth=0):
    preset = presets.get(preset_name)
    if preset is None or depth > 32:
        return None
    if "binaryDir" in preset:
        return preset["binaryDir"]
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        found = binary_dir(parent, depth + 1)
        if found is not None:
            return found
    return None


seen = set()
load(os.path.join(root, "CMakePresets.json"), seen)
load(os.path.join(root, "CMakeUserPresets.json"), seen)
preset = presets.get(name)
value = binary_dir(name) if preset is not None and not preset.get("hidden", False) else None
if value is None:
    sys.exit(1)
macros = {
    "sourceDir": root,
    "sourceParentDir": os.path.dirname(root),
    "sourceDirName": os.path.basename(root),
    "presetName": name,
    "dollar": "$",
}
value = re.sub(r"\$\{(\w+)\}", lambda m: macros.get(m.group(1), m.group(0)), value)
value = re.sub(r"\$p?env\{(\w+)\}", lambda m: os.environ.get(m.group(1), ""), value)
print(os.path.normpath(os.path.join(root, value)))
PY
}

# Where the compile database comes from. debug and relwithdebinfo use build/<type>, which this
# script configures when needed, and write the stripped copy to build/<type>/clang-tidy-compdb, as
# always. Another preset's tree, or a --build-dir, is only read: never configured, and its database
# is stripped into a temporary directory that is removed on exit.
EXTERNAL_COMPDB=false
if [[ -n "$BUILD_DIR_ARG" ]]; then
    if [[ -n "$BUILD_NAME" ]]; then
        echo "Error: give a build type or preset ($BUILD_NAME) or --build-dir, not both" >&2
        exit 1
    fi
    if [[ ! -d "$BUILD_DIR_ARG" ]]; then
        echo "Error: --build-dir $BUILD_DIR_ARG: no such directory" >&2
        exit 1
    fi
    BUILD_DIR="$(cd "$BUILD_DIR_ARG" && pwd)"
    if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
        echo "Error: --build-dir $BUILD_DIR has no compile_commands.json." >&2
        echo "Configure that tree first: cmake --preset <its preset>, or cmake -B $BUILD_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
        exit 1
    fi
    EXTERNAL_COMPDB=true
elif [[ -n "$BUILD_NAME" && "$BUILD_NAME" != "debug" && "$BUILD_NAME" != "relwithdebinfo" ]]; then
    if [[ -z "$PYTHON_EXE" ]]; then
        echo "Error: python3 is needed to look up configure preset '$BUILD_NAME' in CMakePresets.json (or pass --build-dir DIR)" >&2
        exit 1
    fi
    if ! BUILD_DIR="$(resolve_preset_binary_dir "$BUILD_NAME")" || [[ -z "$BUILD_DIR" ]]; then
        echo "Error: Unknown argument: $BUILD_NAME (not an option, a .cpp/.h file, or a configure preset with a binaryDir)" >&2
        usage 1
    fi
    if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
        echo "Error: configure preset '$BUILD_NAME' has no compile database: $BUILD_DIR/compile_commands.json is missing." >&2
        echo "Configure it first: cmake --preset $BUILD_NAME" >&2
        exit 1
    fi
    EXTERNAL_COMPDB=true
else
    BUILD_TYPE="${BUILD_NAME:-$BUILD_TYPE}"
    BUILD_DIR="${PROJECT_ROOT}/build/${BUILD_TYPE}"
fi

COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"
if $EXTERNAL_COMPDB; then
    TIDY_COMPDB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/clang-tidy-compdb.XXXXXX")"
    trap 'rm -rf "$TIDY_COMPDB_DIR"' EXIT
else
    TIDY_COMPDB_DIR="${BUILD_DIR}/clang-tidy-compdb"
fi
COMPILE_COMMANDS_TIDY="${TIDY_COMPDB_DIR}/compile_commands.json"
CONFIG_FILE="${PROJECT_ROOT}/.clang-tidy"

# Write compile database $1 to $2 without the flags clang-tidy can't use: C++20 module @*.modmap
# files and -fmodule-output, and CMake's precompiled-header flags (the binary -include-pch, the
# cmake_pch.hxx force-include and -fno-pch-timestamp), which need a full build to resolve. The same
# patterns as the generate-clang-tidy-compile-commands target (cmake/StaticAnalysis.cmake) and
# tools/clang-tidy.ps1.
sanitize_compile_commands() {
    sed \
        -e 's/@[^ ]*\.modmap//g' \
        -e 's/-fmodule-output=[^ ]*//g' \
        -e 's/-Xclang -include-pch -Xclang [^ ]*//g' \
        -e 's/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g' \
        -e 's/-Xclang -fno-pch-timestamp//g' \
        "$1" > "$2"
}

# shellcheck source=tools/common.sh
source "$SCRIPT_DIR/common.sh"

# Find clang-tidy: the CI-pinned LLVM's (/usr/lib/llvm-23/bin) before an unversioned one on PATH, so
# a machine with only clang-tidy-23 installed (no unversioned symlink) still finds it.
CLANG_TIDY="$(find_llvm_tool clang-tidy || true)"
if [[ -z "$CLANG_TIDY" ]]; then
    echo "Error: clang-tidy not found. Install via: apt install clang-tidy-$(pinned_llvm_major)" >&2
    exit 1
fi
warn_llvm_version_skew "$CLANG_TIDY" clang-tidy

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

if $EXTERNAL_COMPDB; then
    if $VERBOSE; then
        echo "Using compile database: $COMPILE_COMMANDS"
    fi
    if grep -qE -- '-include-pch|cmake_pch|-fno-pch-timestamp|\.modmap|-fmodule-output=' "$COMPILE_COMMANDS"; then
        echo "Stripping module/PCH flags into a temporary copy of $COMPILE_COMMANDS"
    fi
    sanitize_compile_commands "$COMPILE_COMMANDS" "$COMPILE_COMMANDS_TIDY"
else
    # Configure if needed (using CMake presets)
    if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
        echo "Build not configured. Running cmake --preset $BUILD_TYPE..."
        cmake --preset "$BUILD_TYPE"
    fi

    # Ensure compile_commands.json exists (emitted by CMake at generate time)
    if [[ ! -f "$COMPILE_COMMANDS" ]]; then
        echo "compile_commands.json missing. Re-running cmake --preset $BUILD_TYPE..."
        cmake --preset "$BUILD_TYPE"
    fi

    # Build the clean tidy database on every invocation so it stays in sync with the active build.
    # The generate-clang-tidy-compile-commands target writes a sanitized compile_commands.json into
    # ${BUILD_DIR}/clang-tidy-compdb without touching the live database clangd watches.
    if $VERBOSE; then
        echo "Generating clang-tidy compilation database via CMake target..."
    fi
    rm -f "$COMPILE_COMMANDS_TIDY"
    # Don't hard-fail here: older build dirs may lack the target, and the sed fallback below
    # still produces a usable database. But surface the failure instead of discarding it.
    if ! TIDY_COMPDB_OUTPUT=$(cmake --build "$BUILD_DIR" --target generate-clang-tidy-compile-commands 2>&1); then
        echo "Warning: generate-clang-tidy-compile-commands target failed; falling back to sed-sanitized database." >&2
        echo "$TIDY_COMPDB_OUTPUT" | tail -n 20 >&2
    fi

    # If the CMake target didn't produce the file (older build dir or no CLANG_TIDY_EXE at configure
    # time), fall back to generating it here from the live database.
    if [[ ! -f "$COMPILE_COMMANDS_TIDY" ]]; then
        if $VERBOSE; then
            echo "Falling back: writing sanitized compile_commands.json for clang-tidy"
        fi
        mkdir -p "$TIDY_COMPDB_DIR"
        sanitize_compile_commands "$COMPILE_COMMANDS" "$COMPILE_COMMANDS_TIDY"
    fi
fi

# Several targets compile the same src/ files (the TaskSmackApp object library, the tests, ...), and
# clang-tidy analyzes a file once per database entry. Keep one entry per file, the app's (#1626).
# Not fatal: without it every file is still analyzed, just once per target that compiles it.
if [[ -z "$PYTHON_EXE" ]]; then
    echo "Warning: python3 not found; compile database not de-duplicated (files may be analyzed more than once)." >&2
elif ! DEDUPE_OUTPUT=$("$PYTHON_EXE" -I -X utf8 "$SCRIPT_DIR/dedupe-compile-commands.py" "$COMPILE_COMMANDS_TIDY" 2>&1); then
    echo "Warning: compile database not de-duplicated (files may be analyzed more than once):" >&2
    echo "$DEDUPE_OUTPUT" | tail -n 20 >&2
elif $VERBOSE; then
    echo "$DEDUPE_OUTPUT"
fi

COMPILE_COMMANDS_DIR_IN_USE="$TIDY_COMPDB_DIR"

# Determine files to analyze
if [[ ${#FILES[@]} -eq 0 ]]; then
    if [[ "$CHANGED_ONLY" == "true" ]]; then
        # Files this branch changes (committed, staged, unstaged, untracked); see list_changed_files
        mapfile -t CHANGED_FILES < <(list_changed_files "$PROJECT_ROOT" | grep -E '\.(cpp|h)$' | grep -v 'Platform/Windows/' || true)
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

# Each task is "<cache key><TAB><file>"; the key is "-" when the file's result isn't cached.
TASKS=()
if [[ -n "$CACHE_DIR" ]]; then
    # A whole-tree run prunes markers no current file has, so a saved cache holds only live keys.
    PRUNE_ARGS=()
    if [[ ${#FILES[@]} -eq 0 && "$CHANGED_ONLY" != "true" ]]; then
        PRUNE_ARGS=(--prune)
    fi
    # The flags below must match run_clang_tidy's: they are part of every cache key.
    if [[ -n "$PYTHON_EXE" ]] && SELECTED=$("$PYTHON_EXE" -I -X utf8 "$SCRIPT_DIR/tidy-cache.py" select \
        --cache-dir "$CACHE_DIR" --compdb "$COMPILE_COMMANDS_TIDY" --clang-tidy "$CLANG_TIDY" \
        --repo-root "$PROJECT_ROOT" --salt-file "$CONFIG_FILE" --salt-file "${BASH_SOURCE[0]}" \
        --salt="$HEADER_FILTER_REGEX" --salt="$EXCLUDE_HEADER_FILTER_REGEX" \
        --extra-arg=-std=c++23 --extra-arg=-Wno-unknown-warning-option \
        "${PRUNE_ARGS[@]}" -- "${SOURCE_FILES[@]}"); then
        if [[ -n "$SELECTED" ]]; then
            mapfile -t TASKS <<<"$SELECTED"
        fi
        if [[ ${#TASKS[@]} -eq 0 ]]; then
            echo "clang-tidy completed successfully (every file unchanged since a clean run)."
            exit 0
        fi
    else
        echo "Warning: clang-tidy result cache unavailable; analyzing every file." >&2
        CACHE_DIR=""
    fi
fi
if [[ ${#TASKS[@]} -eq 0 ]]; then
    for f in "${SOURCE_FILES[@]}"; do
        TASKS+=("-"$'\t'"$f")
    done
fi

if $VERBOSE; then
    echo "Running clang-tidy on ${#TASKS[@]} files with $JOBS parallel jobs..."
    echo ""
fi

# Function to run clang-tidy on a single "<cache key><TAB><file>" task
# shellcheck disable=SC2317,SC2329  # Function is called via GNU parallel/xargs
run_clang_tidy() {
    local key="${1%%$'\t'*}"
    local file="${1#*$'\t'}"
    local verbose="$2"
    local relative_path="${file#"${PROJECT_ROOT}"/}"

    if [[ "$verbose" == "true" ]]; then
        echo "  Analyzing: $relative_path"
    fi

    local output rc=0
    output=$("$CLANG_TIDY" \
        --config-file="$CONFIG_FILE" \
        --header-filter="$HEADER_FILTER_REGEX" \
        --exclude-header-filter="$EXCLUDE_HEADER_FILTER_REGEX" \
        -p "$COMPILE_COMMANDS_DIR_IN_USE" \
        --extra-arg=-std=c++23 \
        --extra-arg=-Wno-unknown-warning-option \
        "$file" 2>&1) || rc=$?
    if [[ -n "$output" ]]; then
        printf '%s\n' "$output"
    fi
    # Record only a clean result (#1785); a file with findings is analyzed again next time.
    if [[ $rc -eq 0 && "$key" != "-" && -n "$TIDY_CACHE_DIR" ]] && ! grep -qE '(warning|error):' <<<"$output"; then
        printf '%s\n' "$relative_path" > "$TIDY_CACHE_DIR/$key" || true
    fi
    return "$rc"
}
export -f run_clang_tidy
TIDY_CACHE_DIR="$CACHE_DIR"
export CLANG_TIDY CONFIG_FILE BUILD_DIR PROJECT_ROOT HEADER_FILTER_REGEX EXCLUDE_HEADER_FILTER_REGEX COMPILE_COMMANDS_DIR_IN_USE TIDY_CACHE_DIR

# Run clang-tidy in parallel
HAS_ERRORS=0
if command -v parallel &>/dev/null; then
    # Use GNU parallel if available
    if ! printf '%s\n' "${TASKS[@]}" | parallel -j "$JOBS" run_clang_tidy {} "$VERBOSE"; then
        HAS_ERRORS=1
    fi
else
    # Fall back to xargs
    if ! printf '%s\0' "${TASKS[@]}" | xargs -0 -P "$JOBS" -I {} bash -c 'run_clang_tidy "$@"' _ {} "$VERBOSE"; then
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
