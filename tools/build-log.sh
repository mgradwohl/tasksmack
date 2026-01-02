#!/bin/bash
# Build with logging - captures configure and build output to a timestamped log file
# Usage: ./tools/build-log.sh [preset]
# Default preset: debug

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

PRESET="${1:-debug}"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOGDIR="$PROJECT_ROOT/build-logs"
LOGFILE="$LOGDIR/build-${PRESET}-${TIMESTAMP}.log"

# Track build status (will be written to temp file from subshell)
BUILD_STATUS_FILE=$(mktemp)
echo "0" > "$BUILD_STATUS_FILE"

# Create log directory if it doesn't exist
mkdir -p "$LOGDIR"

# Strip ANSI color codes for clean log file
strip_colors() {
    sed 's/\x1b\[[0-9;]*m//g'
}

echo "Building preset: $PRESET"
echo "Log file: $LOGFILE"
echo ""

{
    echo "========================================"
    echo "TaskSmack Build Log"
    echo "========================================"
    echo "Preset:    $PRESET"
    echo "Started:   $(date)"
    echo "Host:      $(hostname)"
    echo "Directory: $PROJECT_ROOT"
    echo ""

    echo "========================================"
    echo "Configure"
    echo "========================================"
    if cmake --preset "$PRESET" 2>&1; then
        echo ""
        echo "[Configure completed successfully]"
    else
        CONFIG_EXIT=$?
        echo ""
        echo "[Configure FAILED with exit code $CONFIG_EXIT]"
        echo "$CONFIG_EXIT" > "$BUILD_STATUS_FILE"
    fi
    echo ""

    echo "========================================"
    echo "Build"
    echo "========================================"
    if cmake --build --preset "$PRESET" 2>&1; then
        echo ""
        echo "[Build completed successfully]"
    else
        BUILD_EXIT=$?
        echo ""
        echo "[Build FAILED with exit code $BUILD_EXIT]"
        echo "$BUILD_EXIT" > "$BUILD_STATUS_FILE"
    fi
    echo ""

    echo "========================================"
    echo "Summary"
    echo "========================================"
    echo "Finished:  $(date)"
    FINAL_STATUS=$(cat "$BUILD_STATUS_FILE")
    if [[ "$FINAL_STATUS" -eq 0 ]]; then
        echo "Status:    SUCCESS"
    else
        echo "Status:    FAILED"
    fi
    echo "========================================"

} 2>&1 | tee >(strip_colors > "$LOGFILE")

echo ""
echo "Log saved to: $LOGFILE"

# Keep only the last 10 log files to avoid clutter
cd "$LOGDIR"
find . -maxdepth 1 -name "build-*.log" -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | tail -n +11 | cut -d' ' -f2- | xargs -r rm --

# Read final status and cleanup
FINAL_EXIT=$(cat "$BUILD_STATUS_FILE")
rm -f "$BUILD_STATUS_FILE"
exit "$FINAL_EXIT"
