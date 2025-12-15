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
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=true; shift ;;
        -h|--help) usage ;;
        *) echo "Error: Unknown argument: $1" >&2; usage ;;
    esac
done

echo "Checking clang-format compliance..."

# Find clang-format
CLANG_FORMAT=""
if command -v clang-format &> /dev/null; then
    CLANG_FORMAT="clang-format"
else
    for ver in 22 21 20 19 18 17; do
        if [[ -x "/usr/lib/llvm-$ver/bin/clang-format" ]]; then
            CLANG_FORMAT="/usr/lib/llvm-$ver/bin/clang-format"
            break
        fi
    done
fi

if [[ -z "$CLANG_FORMAT" ]]; then
    echo "Error: clang-format not found" >&2
    exit 1
fi

FAILED=0
FILE_COUNT=0

while IFS= read -r -d '' file; do
    FILE_COUNT=$((FILE_COUNT + 1))
    if $VERBOSE; then
        echo "Checking: $(basename "$file")"
    fi
    if ! $CLANG_FORMAT --dry-run --Werror "$file" 2>/dev/null; then
        echo "FAIL: $file"
        FAILED=$((FAILED + 1))
    fi
done < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/tests" -type f \( -name "*.cpp" -o -name "*.h" \) ! -path "*/build/*" ! -path "*/.git/*" -print0 2>/dev/null)

if [[ $FAILED -gt 0 ]]; then
    echo "$FAILED of $FILE_COUNT files need formatting."
    echo "Run 'tools/clang-format.sh' to fix."
    exit 1
else
    echo "All $FILE_COUNT files are correctly formatted."
    exit 0
fi
