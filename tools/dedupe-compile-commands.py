#!/usr/bin/env python3
"""
tools/dedupe-compile-commands.py -- Keep one compilation database entry per source file.

Usage:
    tools/dedupe-compile-commands.py <compile_commands.json>

Rewrites the file in place. Used by tools/clang-tidy.sh and tools/clang-tidy.ps1 on their private
copy (build/<preset>/clang-tidy-compdb/), never on the live database clangd watches.

Several targets compile the same src/ files: the TaskSmackApp object library (#1623) and the test
binary (TaskSmackTests). clang-tidy analyzes a file once per database entry ("(1/2) Processing
file", "(2/2) Processing file"), so those files were analyzed twice and the CI static-analysis jobs
ran out of time (#1626). The entry kept is the app's: TaskSmackApp; TaskSmack (main.cpp, and build
trees configured before #1623); TaskSmackUiTraining (src/Training/, #880). Files are therefore
analyzed with the real app flags, not the test binary's. A file none of those compiles keeps its
first entry. Entry order is otherwise preserved.
"""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

# Most preferred first. The object-file path names the target: CMakeFiles/<target>.dir/...
PREFERRED_TARGETS = (
    re.compile(r"[\\/]TaskSmackApp\.dir[\\/]"),
    re.compile(r"[\\/]TaskSmack\.dir[\\/]"),
    re.compile(r"[\\/]TaskSmackUiTraining\.dir[\\/]"),  # src/Training/ (#880): not in the app library
)


def file_key(entry: dict) -> str:
    path = os.path.join(entry.get("directory", ""), entry["file"])
    return os.path.normcase(os.path.normpath(path))


def rank(entry: dict) -> int:
    where = entry.get("output") or entry.get("command") or " ".join(entry.get("arguments", []))
    for index, pattern in enumerate(PREFERRED_TARGETS):
        if pattern.search(where):
            return index
    return len(PREFERRED_TARGETS)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    entries = json.loads(path.read_text(encoding="utf-8-sig"))

    best: dict[str, int] = {}  # file -> index of the entry kept
    for index, entry in enumerate(entries):
        key = file_key(entry)
        if key not in best or rank(entry) < rank(entries[best[key]]):
            best[key] = index
    kept = sorted(best.values())

    if len(kept) != len(entries):
        # Write beside it, then replace: an interrupted run never leaves a truncated database.
        temp = path.with_name(path.name + ".tmp")
        temp.write_text(json.dumps([entries[i] for i in kept], indent=2) + "\n", encoding="utf-8", newline="\n")
        os.replace(temp, path)
    print(f"compile database: kept {len(kept)} of {len(entries)} entries (one per source file)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
