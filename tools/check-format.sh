#!/usr/bin/env bash
# Check clang-format compliance without modifying files
# Usage: ./check-format.sh [OPTIONS]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

VERBOSE=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Check clang-format compliance without modifying files.
Exit code 0 = all files formatted correctly.
Exit code 1 = formatting issues found.

Options:
  -v, --verbose   Show per-file progress
  -h, --help      Show this help
EOF
    exit "${1:-0}"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -h|--help) usage ;;
        *) echo "Error: Unknown argument: $1" >&2; usage 1 ;;
    esac
done

echo "Checking clang-format compliance..."

# shellcheck source=tools/common.sh
source "$SCRIPT_DIR/common.sh"

# Find clang-format: the CI-pinned LLVM's (/usr/lib/llvm-23/bin) before an older unversioned one on PATH
CLANG_FORMAT="$(find_llvm_tool clang-format || true)"
if [[ -z "$CLANG_FORMAT" ]]; then
    echo "Error: clang-format not found. Install via: apt install clang-format-$(pinned_llvm_major)" >&2
    exit 1
fi
warn_llvm_version_skew "$CLANG_FORMAT" clang-format

FAILED=0
FILE_COUNT=0

mapfile -d '' FILES < <(list_format_files "$PROJECT_ROOT")
# None found is never a real result (a filter that matched the checkout's own path once passed with
# "All 0 files", #3528), so it fails.
if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "Error: no C++ sources found under ${PROJECT_ROOT} (${FORMAT_DIRS[*]})." >&2
    exit 1
fi

for file in "${FILES[@]}"; do
    FILE_COUNT=$((FILE_COUNT + 1))
    if $VERBOSE; then
        echo "Checking: $(basename "$file")"
    fi
    if ! "$CLANG_FORMAT" --dry-run --Werror "$file" 2>/dev/null; then
        echo "FAIL: $file"
        FAILED=$((FAILED + 1))
    fi
done

if [[ $FAILED -gt 0 ]]; then
    echo "$FAILED of $FILE_COUNT files need formatting."
    echo "Run 'tools/clang-format.sh' to fix."
    exit 1
else
    echo "All $FILE_COUNT files are correctly formatted."
    exit 0
fi
