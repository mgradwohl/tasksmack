#!/usr/bin/env python3
"""
tools/arm64-paths.py -- Does a change touch the paths that run ARM64 CI (#3930)?

Usage:
    python3 -I tools/arm64-paths.py --labeler <file> --base <commit> [--head <commit>]
    python3 -I tools/arm64-paths.py --labeler <file> --paths <path>...

The pull-request labeler's file (--labeler) holds the globs that add the `ci:arm64` label, in its
`"ci:arm64":` block. arm64.yml runs a pull request when it carries that label, but the labeler adds the
label with GITHUB_TOKEN, and an event made with that token starts no workflow (#3820), so the label
alone can't run a PR's first push. arm64.yml's detect job calls this script instead, with the same
globs, so the labeler's file stays the one place the paths are listed.

Prints `true` when any changed path matches a glob and `false` when none does. With --base the changes
are `git diff --name-only --no-renames <base> <head>` (head defaults to HEAD): a deleted path counts,
and a rename counts as its old and its new path. Exits 2, printing nothing, when the labeler's file has
no `ci:arm64` globs or git fails; the caller treats that as `true`, the safe value that runs ARM64.

Glob rules follow the labeler's (minimatch with dot: true): `*` and `?` stay inside one path segment,
`**` crosses segments, and `**/` also matches no directory at all. Standard library only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

LABEL = "ci:arm64"


def globs_for_label(text: str, label: str = LABEL) -> list[str]:
    """The quoted globs under `"<label>":` in the labeler's file, up to the next top-level key."""
    globs: list[str] = []
    in_block = False
    for line in text.splitlines():
        if re.match(r"^\S", line) and not line.startswith("#"):
            in_block = line.strip() == f'"{label}":'
            continue
        if in_block:
            item = re.match(r'^\s+-\s+"([^"]+)"\s*(?:#.*)?$', line)
            if item:
                globs.append(item.group(1))
    return globs


def glob_regex(glob: str) -> re.Pattern[str]:
    out: list[str] = []
    i = 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif glob.startswith("**", i):
            out.append(".*")
            i += 2
        elif glob[i] == "*":
            out.append("[^/]*")
            i += 1
        elif glob[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(glob[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def matches(paths: list[str], globs: list[str]) -> bool:
    patterns = [glob_regex(glob) for glob in globs]
    return any(pattern.match(path) for path in paths for pattern in patterns)


def changed_paths(base: str, head: str) -> list[str]:
    result = subprocess.run(
        ["git", "diff", "--name-only", "--no-renames", "-z", base, head],
        capture_output=True,
        check=True,
    )
    return [p for p in result.stdout.decode("utf-8", errors="replace").split("\0") if p]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--labeler", required=True, type=Path, help="the pull-request labeler's file")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--base", help="diff base commit")
    source.add_argument("--paths", nargs="+", help="changed paths, instead of a git diff")
    parser.add_argument("--head", default="HEAD")
    args = parser.parse_args(argv)

    try:
        globs = globs_for_label(args.labeler.read_text(encoding="utf-8"))
        paths = args.paths if args.paths is not None else changed_paths(args.base, args.head)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"arm64-paths: {error}", file=sys.stderr)
        return 2
    if not globs:
        print(f"arm64-paths: no `{LABEL}` globs in {args.labeler}", file=sys.stderr)
        return 2
    print("true" if matches(paths, globs) else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
