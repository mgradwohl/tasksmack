#!/usr/bin/env python3
"""
tools/lcov-exclude.py -- Drop structurally untestable lines from an LCOV report, in place.

Usage:
    tools/lcov-exclude.py <coverage.lcov>

llvm-cov has no exclusion markers, so tools/coverage.sh runs this on the LCOV file it hands Codecov;
tools/coverage.ps1 does the same in PowerShell (Remove-ExcludedLcovLine), and the two must agree.
A source line is dropped when:

- it lies inside LCOV_EXCL_START ... LCOV_EXCL_STOP (inclusive) or carries LCOV_EXCL_LINE (#2039).
  Each marker states why no test can run the line, such as a modal shell dialog or a guard that the
  type's invariants make unreachable (#3221);
- it declares a defaulted comparison (`operator==` or `operator<=>` ... `= default;`). Clang folds
  one branch of the synthesized member-wise compare, so the line stays partly covered whatever a
  test does (#3221).

The line's DA and BRDA records go, as do the functions that start on it; each file's LF/LH, BRF/BRH
and FNF/FNH lose what was dropped. They are llvm-cov's own counts, not a count of the records, so
they are reduced rather than recounted. The HTML report and the llvm-cov summary still count every
line. Prints how many line records it dropped.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

DEFAULTED_COMPARISON = re.compile(r"\boperator\s*(?:==|<=>)\s*\(.*=\s*default\s*;")
COUNT_TAGS = ("LF", "LH", "BRF", "BRH", "FNF", "FNH")


def excluded_lines(source: Path) -> set[int]:
    """The 1-based line numbers of @source that the report leaves out."""
    if not source.is_file():
        return set()
    excluded: set[int] = set()
    inside = False
    lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
    for number, text in enumerate(lines, start=1):
        if "LCOV_EXCL_START" in text:
            inside = True
        if inside or "LCOV_EXCL_LINE" in text or DEFAULTED_COMPARISON.search(text):
            excluded.add(number)
        if "LCOV_EXCL_STOP" in text:
            inside = False
    if inside:
        raise ValueError(f"{source}: LCOV_EXCL_START without a matching LCOV_EXCL_STOP")
    return excluded


def filter_records(records: list[str]) -> tuple[list[str], int]:
    """@records without the excluded lines, and how many DA records were dropped."""
    output: list[str] = []
    excluded: set[int] = set()
    dropped_functions: set[str] = set()
    dropped = dict.fromkeys(COUNT_TAGS, 0)
    dropped_lines = 0
    for record in records:
        if record.startswith("SF:"):
            excluded = excluded_lines(Path(record[3:]))
            dropped_functions = set()
            dropped = dict.fromkeys(COUNT_TAGS, 0)
            output.append(record)
            continue
        if not excluded:
            output.append(record)
            continue
        tag, _, value = record.partition(":")
        if tag == "FN":
            line, _, name = value.partition(",")
            if int(line) in excluded:
                dropped_functions.add(name)
                continue
        elif tag == "FNDA":
            hits, _, name = value.partition(",")
            if name in dropped_functions:
                dropped["FNF"] += 1
                dropped["FNH"] += int(hits) > 0
                continue
        elif tag == "BRDA":
            fields = value.split(",")
            if int(fields[0]) in excluded:
                dropped["BRF"] += 1
                dropped["BRH"] += fields[3] != "-" and int(fields[3]) > 0
                continue
        elif tag == "DA":
            fields = value.split(",")
            if int(fields[0]) in excluded:
                dropped_lines += 1
                dropped["LF"] += 1
                dropped["LH"] += int(fields[1]) > 0
                continue
        elif tag in COUNT_TAGS:
            output.append(f"{tag}:{int(value) - dropped[tag]}")
            continue
        output.append(record)
    return output, dropped_lines


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[3], file=sys.stderr)
        return 2
    lcov = Path(argv[1])
    output, dropped_lines = filter_records(lcov.read_text(encoding="utf-8").splitlines())
    lcov.write_text("\n".join(output) + "\n", encoding="utf-8", newline="\n")
    print(dropped_lines)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
