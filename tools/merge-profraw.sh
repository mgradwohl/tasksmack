#!/usr/bin/env bash
# Merges coverage profiles into one .profdata (#4080), as tools/coverage.sh's merge step:
#   merge-profraw.sh LLVM_PROFDATA OUTPUT PROFILE...
# A profile llvm-profdata can't read (a child process ended before its header was written, say) is
# skipped and named, and the rest are merged: one bad file no longer fails a run whose tests all passed.
# It still fails when no profile is given, none can be merged, or the output is missing or empty.
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: merge-profraw.sh LLVM_PROFDATA OUTPUT PROFILE..." >&2
    exit 2
fi
profdata="$1"
output="$2"
shift 2

warnings="$(mktemp)"
trap 'rm -f "${warnings}"' EXIT

status=0
# --failure-mode=all: fail only when every input fails, where the default (any) fails on the first bad one.
"${profdata}" merge -sparse --failure-mode=all "$@" -o "${output}" 2>"${warnings}" || status=$?

skipped=0
while IFS= read -r line; do
    if [[ "${line}" == warning:* ]]; then
        echo "Skipped a profile llvm-profdata couldn't read: ${line#warning: }" >&2
        skipped=$((skipped + 1))
    elif [[ -n "${line}" ]]; then
        echo "${line}" >&2
    fi
done <"${warnings}"

if [[ "${status}" -ne 0 || ! -s "${output}" ]]; then
    echo "Error: no coverage profile could be merged (llvm-profdata exited ${status}; ${skipped} skipped)" >&2
    exit 1
fi
if [[ "${skipped}" -gt 0 ]]; then
    echo "Merged $(($# - skipped)) of $# profiles; ${skipped} skipped" >&2
fi
