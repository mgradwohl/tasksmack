#!/usr/bin/env python3
"""
tools/check-cache-prune-triggers.py -- Fail when a workflow that saves caches doesn't trigger the prune (#2410).

Usage:
    python -I tools/check-cache-prune-triggers.py [--root DIR]

.github/workflows/cache-prune.yml deletes superseded Actions cache entries on main (#1583). It runs on
`workflow_run` after the workflows named in its `workflows:` list, so a workflow that saves caches but is
missing from that list leaves its stale entries in place until some other listed workflow happens to
finish after it (#2410: ARM64's ccache and LLVM entries).

A workflow counts as saving caches when, outside comments, it:

  - uses hendrikmuhs/ccache-action (which saves by default),
  - uses actions/cache or actions/cache/save,
  - passes `mode: save` to one of the repo's cache actions (fetchcontent-cache, cmake-check-cache,
    tidy-result-cache),
  - uses a local action under .github/actions that saves a cache and has no `mode` input (such as
    setup-windows-llvm), or
  - calls a local reusable workflow (`uses: ./.github/workflows/...`) that saves caches.

Reusable workflows (triggered only by `workflow_call`) are not listed themselves: workflow_run sees
their callers. Every other cache-saving workflow's top-level `name:` must be in cache-prune's list.
This is a text scan, not a YAML parse, so it needs nothing beyond the standard library.

Run by pre-commit (local hook `check-cache-prune-triggers`). `--root` points at another tree, for
testing. Exits 1 on any failure, 0 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

WORKFLOWS_DIR = Path(".github/workflows")
ACTIONS_DIR = Path(".github/actions")
PRUNE_WORKFLOW = "cache-prune.yml"

CCACHE_ACTION = re.compile(r"^\s*-?\s*uses:\s*['\"]?hendrikmuhs/ccache-action@")
CACHE_SAVE_ACTION = re.compile(r"^\s*-?\s*uses:\s*['\"]?actions/cache(/save)?@")
MODE_SAVE = re.compile(r"^\s*mode:\s*['\"]?save['\"]?\s*(#.*)?$")
LOCAL_ACTION = re.compile(r"^\s*-?\s*uses:\s*['\"]?\./\.github/actions/([^'\"\s@#]+?)/?['\"]?\s*(#.*)?$")
LOCAL_WORKFLOW = re.compile(r"^\s*-?\s*uses:\s*['\"]?\./\.github/workflows/([^'\"\s@#]+)['\"]?\s*(#.*)?$")
TOP_LEVEL_KEY = re.compile(r"^([A-Za-z_][\w-]*|\"on\"|'on'):(.*)$")


def read_lines(path: Path) -> list[str]:
    """Returns the file's lines with full-line comments dropped."""
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    return [line for line in text.split("\n") if not line.lstrip().startswith("#")]


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def strip_trailing_comment(value: str) -> str:
    return re.sub(r"\s+#.*$", "", value).strip()


def top_level_name(lines: list[str]) -> str | None:
    for line in lines:
        match = TOP_LEVEL_KEY.match(line)
        if match and match.group(1) == "name":
            return unquote(strip_trailing_comment(match.group(2)))
    return None


def triggers(lines: list[str]) -> set[str]:
    """The event names under the top-level `on:` key."""
    events: set[str] = set()
    in_on = False
    indent: int | None = None  # the event keys' indentation, from the first line under `on:`
    for line in lines:
        match = TOP_LEVEL_KEY.match(line)
        if match:
            in_on = unquote(match.group(1)) == "on"
            indent = None
            if in_on:
                inline = strip_trailing_comment(match.group(2))
                if inline.startswith("["):
                    events.update(unquote(e) for e in inline.strip("[]").split(",") if e.strip())
                elif inline:
                    events.add(unquote(inline))
            continue
        if in_on and line.strip():
            event = re.match(r"^(\s+)(?:-\s*)?([A-Za-z_]+)", line)
            if event and (indent is None or len(event.group(1)) == indent):
                indent = len(event.group(1))
                events.add(event.group(2))
    return events


def saving_local_actions(root: Path) -> set[str]:
    """Local actions that save a cache whenever they run (they have no `mode` input to choose)."""
    saving: set[str] = set()
    actions_dir = root / ACTIONS_DIR
    if not actions_dir.is_dir():
        return saving
    for action in sorted(actions_dir.iterdir()):
        for filename in ("action.yml", "action.yaml"):
            path = action / filename
            if not path.is_file():
                continue
            lines = read_lines(path)
            saves = any(CACHE_SAVE_ACTION.match(line) for line in lines)
            has_mode_input = any(re.match(r"^  mode:\s*$", line) for line in lines)
            if saves and not has_mode_input:
                saving.add(action.name)
    return saving


def saves_directly(lines: list[str], actions: set[str]) -> bool:
    for line in lines:
        if CCACHE_ACTION.match(line) or CACHE_SAVE_ACTION.match(line) or MODE_SAVE.match(line):
            return True
        local = LOCAL_ACTION.match(line)
        if local and local.group(1) in actions:
            return True
    return False


def prune_workflow_list(lines: list[str]) -> list[str] | None:
    """The `workflows:` list under cache-prune's `workflow_run:` trigger, flow or block style."""
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)workflows:(.*)$", line)
        if not match:
            continue
        inline = strip_trailing_comment(match.group(2))
        if inline.startswith("["):
            return [unquote(e) for e in inline.strip("[]").split(",") if e.strip()]
        names: list[str] = []
        indent = len(match.group(1))
        for item in lines[index + 1 :]:
            if not item.strip():
                continue
            item_match = re.match(r"^(\s*)-\s*(.*)$", item)
            if not item_match or len(item_match.group(1)) < indent:
                break
            names.append(unquote(strip_trailing_comment(item_match.group(2))))
        return names
    return None


def check(root: Path) -> list[str]:
    errors: list[str] = []
    workflows_dir = root / WORKFLOWS_DIR
    prune_path = workflows_dir / PRUNE_WORKFLOW
    if not prune_path.is_file():
        return [f"{WORKFLOWS_DIR.as_posix()}/{PRUNE_WORKFLOW}: missing"]
    listed = prune_workflow_list(read_lines(prune_path))
    if listed is None:
        return [f"{WORKFLOWS_DIR.as_posix()}/{PRUNE_WORKFLOW}: no `workflows:` list under workflow_run"]

    actions = saving_local_actions(root)
    files = sorted(p for p in workflows_dir.iterdir() if p.suffix in (".yml", ".yaml") and p.name != PRUNE_WORKFLOW)
    parsed = {path.name: read_lines(path) for path in files}

    # A reusable workflow saves if it does so itself or calls another one that does (to a fixed point).
    saving = {name for name, lines in parsed.items() if saves_directly(lines, actions)}
    changed = True
    while changed:
        changed = False
        for name, lines in parsed.items():
            if name in saving:
                continue
            for line in lines:
                called = LOCAL_WORKFLOW.match(line)
                if called and called.group(1) in saving:
                    saving.add(name)
                    changed = True
                    break

    for name in sorted(saving):
        lines = parsed[name]
        if triggers(lines) == {"workflow_call"}:
            continue  # reusable only: its callers are what workflow_run sees
        workflow_name = top_level_name(lines)
        path = f"{WORKFLOWS_DIR.as_posix()}/{name}"
        if workflow_name is None:
            errors.append(f"{path}: saves caches but has no top-level `name:` for cache-prune to match")
        elif workflow_name not in listed:
            errors.append(
                f"{path}: saves caches, but its name '{workflow_name}' is not in "
                f"{WORKFLOWS_DIR.as_posix()}/{PRUNE_WORKFLOW}'s workflow_run `workflows:` list, so its "
                f"superseded entries are never pruned after it runs (#2410)"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"check-cache-prune-triggers: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
