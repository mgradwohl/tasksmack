#!/usr/bin/env bash
# tools/time-trace.sh — Collect Clang -ftime-trace JSON files and open them in chrome://tracing.
#
# Requires a build configured with -DTASKSMACK_ENABLE_TIME_TRACE=ON.
#
# Usage:
#   ./tools/time-trace.sh [preset]     # Default preset: debug
#   ./tools/time-trace.sh --list       # List discovered trace files without opening
#
# Output: trace JSON files are at build/<preset>/**/*.json (emitted by Clang alongside .o files).
# A merged trace is written to build/<preset>/time-trace-merged.json for easy viewing.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PRESET="${1:-debug}"
LIST_ONLY=false

if [[ "${PRESET}" == "--list" ]]; then
    LIST_ONLY=true
    PRESET="debug"
fi

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "Error: Build directory not found: $BUILD_DIR" >&2
    echo "Configure first: cmake --preset $PRESET -DTASKSMACK_ENABLE_TIME_TRACE=ON" >&2
    exit 1
fi

# Collect all .json trace files (exclude compile_commands*.json and CMake cache files)
mapfile -t TRACE_FILES < <(find "$BUILD_DIR" -name "*.json" -not -name "compile_commands*" \
    -not -path "*/CMakeFiles/*" -not -name "CMakeCache*" 2>/dev/null | sort)

if [[ ${#TRACE_FILES[@]} -eq 0 ]]; then
    echo "No -ftime-trace JSON files found in $BUILD_DIR." >&2
    echo "Make sure the build was configured with -DTASKSMACK_ENABLE_TIME_TRACE=ON" >&2
    exit 1
fi

echo "Found ${#TRACE_FILES[@]} trace file(s) in $BUILD_DIR:"
for f in "${TRACE_FILES[@]}"; do
    echo "  ${f#"${BUILD_DIR}/"}"
done

if $LIST_ONLY; then
    exit 0
fi

# Merge all trace events into a single JSON array for chrome://tracing.
MERGED="${BUILD_DIR}/time-trace-merged.json"
echo ""
echo "Merging into: $MERGED"
python3 - "$MERGED" "${TRACE_FILES[@]}" <<'PYTHON'
import json, sys, pathlib

out_path = sys.argv[1]
input_files = sys.argv[2:]

all_events = []
for path in input_files:
    try:
        data = json.loads(pathlib.Path(path).read_text())
        events = data.get("traceEvents", []) or data if isinstance(data, list) else []
        all_events.extend(events)
    except Exception as e:
        print(f"  Warning: skipping {path}: {e}", file=sys.stderr)

result = {"traceEvents": all_events, "displayTimeUnit": "ms"}
pathlib.Path(out_path).write_text(json.dumps(result, indent=2))
print(f"Merged {len(all_events)} events from {len(input_files)} files.")
PYTHON

echo ""
echo "Open the merged trace in your browser:"
echo "  1. Go to chrome://tracing (or edge://tracing)"
echo "  2. Click 'Load' and select: $MERGED"
echo ""
echo "Alternatively, use Perfetto UI: https://ui.perfetto.dev"

# Try to open automatically
if command -v xdg-open &>/dev/null; then
    xdg-open "$MERGED" 2>/dev/null || true
elif command -v open &>/dev/null; then
    open "$MERGED" 2>/dev/null || true
fi
