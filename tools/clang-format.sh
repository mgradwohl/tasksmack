#!/usr/bin/env bash
# Apply clang-format to source files
# Uses .clang-format configuration from project root
# Usage: ./clang-format.sh [OPTIONS]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

VERBOSE=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Apply clang-format to all source files (in-place).
Use check-format.sh to check without modifying files.

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

echo "Applying clang-format to source files..."

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

# Count files for progress
FILE_COUNT=$(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/tests" -type f \( -name "*.cpp" -o -name "*.h" \) ! -path "*/build/*" ! -path "*/.git/*" 2>/dev/null | wc -l)
CHECKED_COUNT=0

while IFS= read -r -d '' file; do
    CHECKED_COUNT=$((CHECKED_COUNT + 1))
    if $VERBOSE; then
        echo "[$CHECKED_COUNT/$FILE_COUNT] Formatting: $(basename "$file")"
    fi
    $CLANG_FORMAT -i "$file"
done < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/tests" -type f \( -name "*.cpp" -o -name "*.h" \) ! -path "*/build/*" ! -path "*/.git/*" -print0 2>/dev/null)

echo "Formatted $FILE_COUNT files."
