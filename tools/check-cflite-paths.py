#!/usr/bin/env python3
"""
tools/check-cflite-paths.py -- Fail when the ClusterFuzzLite baseline build and PR fuzzing watch different paths (#2533).

Usage:
    python -I tools/check-cflite-paths.py [--root DIR]

.github/workflows/cflite_pr.yml fuzzes a pull request in code-change mode against the baseline build
that .github/workflows/cflite_build.yml uploads from main. CONTRIBUTING.md promises that pushes to main
touching the paths the PR job watches refresh that baseline. GitHub Actions can't share a `paths:` filter
between two workflow files, so the two lists are kept by hand, and this check fails when they drift:
a path only the PR job lists leaves the baseline stale after a merge that touches it, so a later PR is
blamed for a crash already on main (#2533).

The check compares cflite_pr.yml's `on: pull_request: paths:` list with cflite_build.yml's
`on: push: paths:` list as sets (order and quoting don't matter). This is a text scan of block-style
lists, not a YAML parse, so it needs nothing beyond the standard library.

Run by pre-commit (local hook `check-cflite-paths`). `--root` points at another tree, for testing.
Exits 1 on any failure, 0 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

WORKFLOWS_DIR = Path(".github/workflows")
PR_WORKFLOW = "cflite_pr.yml"
BUILD_WORKFLOW = "cflite_build.yml"


def read_lines(path: Path) -> list[str]:
    """Returns the file's lines with full-line comments dropped."""
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    return [line for line in text.split("\n") if not line.lstrip().startswith("#")]


def unquote(value: str) -> str:
    value = re.sub(r"\s+#.*$", "", value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def trigger_paths(lines: list[str], event: str) -> list[str] | None:
    """The block-style `paths:` list under the top-level `on:` key's `event:` entry, or None if absent."""
    in_on = False
    event_indent: int | None = None
    for index, line in enumerate(lines):
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            in_on = re.match(r"^(on|\"on\"|'on'):\s*(#.*)?$", line) is not None
            event_indent = None
            continue
        if not in_on:
            continue
        if event_indent is not None and indent <= event_indent:
            event_indent = None  # left the event's block
        if event_indent is None:
            if re.match(rf"^\s+{re.escape(event)}:\s*(#.*)?$", line):
                event_indent = indent
            continue
        match = re.match(r"^(\s+)paths:\s*(#.*)?$", line)
        if not match:
            continue
        paths: list[str] = []
        paths_indent = len(match.group(1))
        for item in lines[index + 1 :]:
            if not item.strip():
                continue
            item_match = re.match(r"^(\s*)-\s*(.*)$", item)
            if not item_match or len(item_match.group(1)) < paths_indent:
                break
            paths.append(unquote(item_match.group(2)))
        return paths
    return None


def check(root: Path) -> list[str]:
    errors: list[str] = []
    lists: dict[str, list[str]] = {}
    for name, event in ((PR_WORKFLOW, "pull_request"), (BUILD_WORKFLOW, "push")):
        path = root / WORKFLOWS_DIR / name
        display = f"{WORKFLOWS_DIR.as_posix()}/{name}"
        if not path.is_file():
            errors.append(f"{display}: missing")
            continue
        paths = trigger_paths(read_lines(path), event)
        if not paths:
            errors.append(f"{display}: no block-style `paths:` list under `on: {event}:`")
            continue
        lists[name] = paths
    if errors:
        return errors

    pr_paths = set(lists[PR_WORKFLOW])
    build_paths = set(lists[BUILD_WORKFLOW])
    for missing in sorted(pr_paths - build_paths):
        errors.append(
            f"{WORKFLOWS_DIR.as_posix()}/{BUILD_WORKFLOW}: '{missing}' is in {PR_WORKFLOW}'s paths but not here, "
            f"so a push to main touching it leaves the fuzzing baseline stale (#2533)"
        )
    for extra in sorted(build_paths - pr_paths):
        errors.append(
            f"{WORKFLOWS_DIR.as_posix()}/{PR_WORKFLOW}: '{extra}' is in {BUILD_WORKFLOW}'s paths but not here, "
            f"so pull requests touching it are never fuzzed (#2533)"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"check-cflite-paths: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
