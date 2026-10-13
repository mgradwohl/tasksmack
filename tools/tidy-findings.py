#!/usr/bin/env python3
"""
tools/tidy-findings.py -- Count clang-tidy findings by check, as Markdown for a job summary.

Usage:
    tools/tidy-findings.py <clang-tidy output> [--title TEXT]

Reads the output of tools/clang-tidy.sh or tools/clang-tidy.ps1 and prints a Markdown table of
unique findings per check (#1785). A header finding that several translation units report counts
once. Diagnostics without a check name (compiler errors) count under "(compiler)".
Always exits 0: the clang-tidy step itself decides pass or fail.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

FINDING_RE = re.compile(r"^(?P<loc>.+?:\d+:\d+): (?:warning|error): (?P<msg>.*?)(?: \[(?P<checks>[^\]\s]+)\])?\s*$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", help="the saved clang-tidy output")
    parser.add_argument("--title", default="clang-tidy findings")
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
