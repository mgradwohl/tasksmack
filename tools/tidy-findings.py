#!/usr/bin/env python3
"""
tools/tidy-findings.py -- Count clang-tidy findings by check, as Markdown for a job summary.

Usage:
    tools/tidy-findings.py <clang-tidy output> [--title TEXT]
                           [--cache-key KEY --cache-fresh true|false]

Reads the output of tools/clang-tidy.sh or tools/clang-tidy.ps1 and prints a Markdown table of
unique findings per check (#1785). A header finding that several translation units report counts
once. Diagnostics without a check name (compiler errors) count under "(compiler)".

With --cache-key (the key .github/actions/tidy-result-cache restored, empty when none matched) it
also prints one line about the per-file result cache under the title (#3101): which baseline was
restored, or why there was none, and how many translation units were selected, reused from it and
analyzed, from tools/tidy-cache.py's "clang-tidy cache: ..." line in the output.
Always exits 0: the clang-tidy step itself decides pass or fail.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

FINDING_RE = re.compile(r"^(?P<loc>.+?:\d+:\d+): (?:warning|error): (?P<msg>.*?)(?: \[(?P<checks>[^\]\s]+)\])?\s*$")
# tools/tidy-cache.py's summary on stderr, which the tidy scripts pass through to the saved output.
CACHE_COUNTS_RE = re.compile(
    r"clang-tidy cache: (?P<reused>\d+) of (?P<selected>\d+) translation units\b.*?, (?P<analyzed>\d+) to analyze"
)
CACHE_UNAVAILABLE = "clang-tidy result cache unavailable"


def cache_line(text: str, key: str, fresh: bool) -> str:
    """One Markdown line: the restored baseline (or why there is none) and the selected/reused/analyzed counts."""
    if fresh:
        baseline = "none restored (a fresh run: every file is analyzed and the cache re-seeded)"
    elif key:
        baseline = f"`{key}`"
    else:
        # A run reads only its own ref's entries and the default branch's (a pull request: the base branch's).
        baseline = (
            "none found (no entry on this ref or main: none saved yet, unused for 7 days, "
            "or evicted while the repository was over its cache limit)"
        )
    match = CACHE_COUNTS_RE.search(text)
    if match:
        selected, reused, analyzed = (int(match[name]) for name in ("selected", "reused", "analyzed"))
        counts = f"{selected} selected, {reused} reused, {analyzed} analyzed"
        if key and not fresh and selected and not reused:
            counts += (
                " (nothing reused: a new runner image, clang-tidy build, `.clang-tidy` or tidy script, "
                "or a change every file includes, changes every key)"
            )
    elif CACHE_UNAVAILABLE in text:
        counts = "the cache couldn't key the files, so every file was analyzed (see the clang-tidy step's log)"
    else:
        counts = "no cache counts in the output"
    return f"**Result cache:** baseline {baseline}; {counts}."


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", help="the saved clang-tidy output")
    parser.add_argument("--title", default="clang-tidy findings")
    parser.add_argument("--cache-key", help="the result-cache key restored, empty when none matched")
    parser.add_argument("--cache-fresh", default="false", help="'true' when the run ignored the cache")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")

    try:
        text = Path(args.output).read_text(encoding="utf-8-sig", errors="replace")
    except OSError:
        print(f"### {args.title}\n\nNo clang-tidy output (the step didn't run).")
        return 0

    seen: set[tuple[str, str]] = set()
    counts: Counter[str] = Counter()
    for line in text.splitlines():
        match = FINDING_RE.match(line.strip())
        if not match:
            continue
        # "[check-name,-warnings-as-errors]": the first entry is the check.
        check = (match["checks"] or "(compiler)").split(",")[0]
        key = (match["loc"].replace("\\", "/").lower(), check)
        if key not in seen:
            seen.add(key)
            counts[check] += 1

    print(f"### {args.title}\n")
    if args.cache_key is not None:
        print(cache_line(text, args.cache_key.strip(), args.cache_fresh.strip().lower() == "true") + "\n")
    if not counts:
        print("No findings.")
        return 0
    print(f"**{sum(counts.values())}** unique finding(s) from {len(counts)} check(s):\n")
    print("| Check | Findings |\n|---|---:|")
    for check, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        print(f"| `{check}` | {count} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
