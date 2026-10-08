#!/usr/bin/env python3
"""
tools/tidy-changed-files.py -- Pick the translation units clang-tidy must check for a change.

Usage:
    tools/tidy-changed-files.py --base <commit> --platform linux|windows [--head <commit>]

Prints either the single word ALL (run the full analysis) or the repo-relative src/**/*.cpp files
to analyze, one per line (possibly none). Used by ci.yml's pull-request clang-tidy jobs (#1406);
the nightly CI run still analyzes every file.

A full run is required when anything that changes how *every* file is analyzed changed: the
clang-tidy config, the tidy scripts, CMake files/presets (compile flags), this script, ci.yml, or
when a header was deleted or renamed. Otherwise the result is every changed .cpp under src/, plus
every src/ .cpp that includes a changed src/ header directly or through other src/ headers.
Includes are matched by file name, so the include set can only be too large, never too small.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

FULL_RUN_PATTERNS = (
    re.compile(r"^\.clang-tidy$"),
    re.compile(r"^(src/.*/)?CMakeLists\.txt$"),  # not tests/ or benchmarks/: tidy checks src/ only
    re.compile(r"^CMakePresets\.json$"),
    re.compile(r"^cmake/"),
    re.compile(r"^tools/clang-tidy\.(sh|ps1)$"),
    re.compile(r"^tools/common\.(sh|ps1)$"),
    re.compile(r"^tools/tidy-changed-files\.py$"),
    re.compile(r"^\.github/workflows/ci\.yml$"),
)
HEADER_SUFFIXES = (".h", ".hpp", ".inl")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True, help="commit to diff against (the PR's base)")
    parser.add_argument("--head", default="HEAD", help="commit with the change (default: HEAD)")
    parser.add_argument("--platform", required=True, choices=("linux", "windows"))
    args = parser.parse_args()

    other_platform = "src/Platform/Windows/" if args.platform == "linux" else "src/Platform/Linux/"

    status = git("diff", "--name-status", "--no-renames", args.base, args.head)
    changed: list[str] = []
    for line in status.splitlines():
        kind, _, path = line.partition("\t")
        path = path.replace("\\", "/")
        if any(p.search(path) for p in FULL_RUN_PATTERNS):
            print(f"full run: {path} changed", file=sys.stderr)
            print("ALL")
            return 0
        if kind.startswith("D") and path.startswith("src/") and path.endswith(HEADER_SUFFIXES):
            print(f"full run: header {path} was removed", file=sys.stderr)
            print("ALL")
            return 0
        if not kind.startswith("D"):
            changed.append(path)

    src_files = [p.replace("\\", "/") for p in git("ls-files", "src").splitlines()]
    src_files = [p for p in src_files if not p.startswith(other_platform)]
    tus = {p for p in src_files if p.endswith(".cpp")}

    selected = {p for p in changed if p in tus}

    # Names of changed src/ headers, then grow the set through headers that include them.
    dirty_names = {PurePosixPath(p).name for p in changed if p.startswith("src/") and p.endswith(HEADER_SUFFIXES)}
    if dirty_names:
        includes: dict[str, set[str]] = {}
        for path in src_files:
            if path.endswith((".cpp", *HEADER_SUFFIXES)):
                try:
                    text = Path(path).read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                includes[path] = {PurePosixPath(inc).name for inc in INCLUDE_RE.findall(text)}
        grew = True
        while grew:
            grew = False
            for path, names in includes.items():
                if path.endswith(HEADER_SUFFIXES) and PurePosixPath(path).name not in dirty_names and names & dirty_names:
                    dirty_names.add(PurePosixPath(path).name)
                    grew = True
        selected |= {p for p in tus if includes.get(p, set()) & dirty_names}

    for path in sorted(selected):
        print(path)
    print(f"{len(selected)} of {len(tus)} translation units selected", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
