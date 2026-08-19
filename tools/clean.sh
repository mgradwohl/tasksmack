#!/usr/bin/env bash
# tools/clean.sh — Remove TaskSmack build artifacts and caches.
#
# Usage:
#   ./tools/clean.sh              # Remove build/ and coverage/ only (safe for incremental rebuilds)
#   ./tools/clean.sh --all        # Also remove .cache/ (FetchContent downloads — re-fetched on next build)
#   ./tools/clean.sh --dry-run    # Print what would be removed without deleting anything
#
# Pass --yes to skip the confirmation prompt.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CLEAN_ALL=false
DRY_RUN=false
YES=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Remove TaskSmack build artifacts and caches.

Options:
  --all        Also remove .cache/ (FetchContent/CPM downloads)
  --dry-run    Print what would be removed without deleting anything
  --yes        Skip the confirmation prompt
  -h, --help   Show this help
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)    CLEAN_ALL=true; shift ;;
        --dry-run) DRY_RUN=true; shift ;;
        --yes|-y)  YES=true; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

TARGETS=(
    "${REPO_ROOT}/build"
    "${REPO_ROOT}/coverage"
    "${REPO_ROOT}/dist"
    "${REPO_ROOT}/compile_commands.json"
    "${REPO_ROOT}/compile_commands_notidy.json"
)
if $CLEAN_ALL; then
    TARGETS+=("${REPO_ROOT}/.cache")
    TARGETS+=("${REPO_ROOT}/profiles")
fi

echo "The following paths will be removed:"
for t in "${TARGETS[@]}"; do
    if [[ -e "$t" ]]; then
        echo "  $t"
    fi
done
echo ""

if ! $YES && ! $DRY_RUN; then
    read -r -p "Proceed? [y/N] " answer
    case "$answer" in
        [yY][eE][sS]|[yY]) ;;
        *) echo "Aborted."; exit 0 ;;
    esac
fi

for t in "${TARGETS[@]}"; do
    if [[ -e "$t" ]]; then
        if $DRY_RUN; then
            printf '[dry-run] rm -rf "%s"\n' "$t"
        else
            echo "Removing: $t"
            rm -rf "$t"
        fi
    fi
done

if $DRY_RUN; then
    echo "Dry run complete. Nothing was removed."
else
    echo "Clean complete."
fi
