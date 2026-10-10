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
Configure-time templates count as the header they generate: a changed src/**/X.h.in dirties X.h,
and the embedded fallback theme (assets/themes/arctic-fire.toml) dirties FallbackTheme.h.
Includes are matched by file name, so the include set can only be too large, never too small.

The one CMake exception: a change to the root or a src/ CMakeLists.txt whose only added and removed
lines are source/header list entries (src/X.cpp, src/X.h, src/X.hpp, optionally quoted or prefixed
with ${CMAKE_CURRENT_SOURCE_DIR}/) inside a set(<..SOURCES|HEADERS..> or list(APPEND <..>) block,
plus blank and comment lines, does not force a full run: every feature PR that adds a file edits the
root CMakeLists.txt, and a whole-repo run no longer fits the CI job (#1626). Newly listed .cpp files
are selected (subject to the same platform filter); newly listed headers are already covered by the
include scan. Any other CMakeLists.txt change (options, definitions, targets, set() of non-list
variables, an added or deleted CMakeLists.txt, an entry outside a source list) still means ALL.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

FULL_RUN_PATTERNS = (
    re.compile(r"^\.clang-tidy$"),
    re.compile(r"^CMakePresets\.json$"),
    re.compile(r"^cmake/"),
    re.compile(r"^tools/clang-tidy\.(sh|ps1)$"),
    re.compile(r"^tools/common\.(sh|ps1)$"),
    re.compile(r"^tools/tidy-changed-files\.py$"),
    re.compile(r"^\.github/workflows/ci\.yml$"),
    # The tidy jobs' toolchain: the LLVM/clang-tidy and GLAD setup actions and the GLAD generator's
    # pinned requirements. A PR that changes only these must still exercise them.
    re.compile(r"^\.github/actions/(setup-llvm|setup-windows-llvm|setup-python-glad)/"),
    re.compile(r"^requirements-glad\.(in|txt)$"),
)
# Not tests/ or benchmarks/: tidy checks src/ only. A full run unless list_only_cmake_change() says no.
CMAKE_LISTS_RE = re.compile(r"^(src/.*/)?CMakeLists\.txt$")
HEADER_SUFFIXES = (".h", ".hpp", ".inl")
# A source-list block opener with nothing else on the line: set(TASKSMACK_SOURCES / list(APPEND X_HEADERS
LIST_OPENER_RE = re.compile(
    r"^\s*(?:set\s*\(|list\s*\(\s*APPEND\s)\s*[A-Za-z0-9_]*(?:SOURCES|HEADERS)[A-Za-z0-9_]*\s*(?:#.*)?$",
    re.IGNORECASE,
)
# One plain list entry per line: a .cpp/.h/.hpp path, optionally quoted or ${CMAKE_CURRENT_SOURCE_DIR}/-prefixed.
LIST_ENTRY_RE = re.compile(
    r'^\s*("?)(?:\$\{CMAKE_CURRENT_SOURCE_DIR\}/)?([A-Za-z0-9_./+-]+\.(?:cpp|h|hpp))\1\s*(?:#.*)?$'
)
# Blank or line-comment-only. Not "#[[" / "#[=[": a bracket comment can comment out real code.
BLANK_OR_COMMENT_RE = re.compile(r"^\s*(?:#(?!\[=*\[).*)?$")
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
# Non-header inputs that CMakeLists.txt turns into generated headers (configure_file).
GENERATED_HEADER_INPUTS = {"assets/themes/arctic-fire.toml": "FallbackTheme.h"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], check=True, capture_output=True, text=True, encoding="utf-8", errors="replace"
    ).stdout


def in_source_list(lines: list[str], index: int) -> bool:
    """True when lines[index] sits inside a set(..SOURCES|HEADERS..)/list(APPEND ..) block: the
    nearest preceding line that is not a list entry, blank or comment opens such a block."""
    for line in reversed(lines[:index]):
        if LIST_ENTRY_RE.match(line) or BLANK_OR_COMMENT_RE.match(line):
            continue
        return bool(LIST_OPENER_RE.match(line))
    return False


def list_only_cmake_change(base: str, head: str, path: str) -> set[str] | None:
    """For a modified CMakeLists.txt, the repo-relative files its change added to a source list, or
    None when the change is anything but source/header list entries (and blank/comment lines)."""
    directory = posixpath.dirname(path)
    try:
        old_lines = git("show", f"{base}:{path}").replace("\r", "").split("\n")
        new_lines = git("show", f"{head}:{path}").replace("\r", "").split("\n")
        diff = git("diff", "-U0", "--no-color", "--no-ext-diff", "--no-renames", base, head, "--", path)
    except subprocess.CalledProcessError:
        return None
    added: set[str] = set()
    old_no = new_no = 0
    in_hunk = False
    for raw in diff.split("\n"):
        line = raw.rstrip("\r")
        if hunk := HUNK_RE.match(line):
            old_no, new_no = int(hunk.group(1)), int(hunk.group(3))
            in_hunk = True
            continue
        if not in_hunk or not line or line.startswith("\\"):
            continue  # file header, trailing empty split, "\ No newline at end of file"
        sign, text = line[0], line[1:]
        if sign == "-":
            lines, index = old_lines, old_no - 1
            old_no += 1
        elif sign == "+":
            lines, index = new_lines, new_no - 1
            new_no += 1
        else:
            return None  # unexpected in a -U0 diff: be conservative
        if BLANK_OR_COMMENT_RE.match(text):
            continue
        entry = LIST_ENTRY_RE.match(text)
        if not entry or not in_source_list(lines, index):
            return None
        resolved = posixpath.normpath(posixpath.join(directory, entry.group(2)))
        if not resolved.startswith("src/"):
            return None
        if sign == "+":
            added.add(resolved)
    return added


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True, help="commit to diff against (the PR's base)")
    parser.add_argument("--head", default="HEAD", help="commit with the change (default: HEAD)")
    parser.add_argument("--platform", required=True, choices=("linux", "windows"))
    args = parser.parse_args()
    # The workflow compares the first line with "ALL"; never emit CRLF, even on Windows.
    sys.stdout.reconfigure(newline="\n")

    other_platform = "src/Platform/Windows/" if args.platform == "linux" else "src/Platform/Linux/"

    status = git("diff", "--name-status", "--no-renames", args.base, args.head)
    changed: list[str] = []
    listed: set[str] = set()  # files newly added to a CMake source list
    for line in status.splitlines():
        kind, _, path = line.partition("\t")
        path = path.replace("\\", "/")
        if CMAKE_LISTS_RE.search(path):
            entries = list_only_cmake_change(args.base, args.head, path) if kind.startswith("M") else None
            if entries is None:
                print(f"full run: {path} changed beyond source-list entries", file=sys.stderr)
                print("ALL")
                return 0
            listed |= entries
            continue
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

    selected = {p for p in changed if p in tus} | (listed & tus)

    # Names of changed src/ headers, then grow the set through headers that include them.
    dirty_names = {PurePosixPath(p).name for p in changed if p.startswith("src/") and p.endswith(HEADER_SUFFIXES)}
    dirty_names |= {PurePosixPath(p).name[: -len(".in")] for p in changed if p.startswith("src/") and p.endswith(".h.in")}
    dirty_names |= {GENERATED_HEADER_INPUTS[p] for p in changed if p in GENERATED_HEADER_INPUTS}
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
