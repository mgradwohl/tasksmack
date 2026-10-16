#!/usr/bin/env python3
"""
tools/check-guidance-sync.py -- Fail when the AI-agent guidance copies drift apart (#2246).

Usage:
    python -I tools/check-guidance-sync.py [--root DIR]

AGENTS.md is the canonical agent guidance and .github/skills/code-review/SKILL.md the canonical review
checklist. This checks that:

  1. .claude/skills/tasksmack-review/SKILL.md has the same body as the canonical SKILL.md. Each file's
     YAML frontmatter (the leading `---` ... `---` block) is ignored, since the two skills differ in
     name and description. Line endings are normalised before comparing.
  2. CLAUDE.md still imports AGENTS.md with an `@AGENTS.md` line, and AGENTS.md exists.

Run by pre-commit (local hook `check-guidance-sync`). `--root` points at another tree, for testing.
Exits 1 on any failure, 0 otherwise.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

CANONICAL_SKILL = ".github/skills/code-review/SKILL.md"
MIRROR_SKILLS = (".claude/skills/tasksmack-review/SKILL.md",)
CLAUDE_MD = "CLAUDE.md"
AGENTS_MD = "AGENTS.md"
IMPORT_LINE = "@AGENTS.md"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def strip_frontmatter(text: str) -> str:
    """Returns `text` without a leading `---` ... `---` frontmatter block."""
    lines = text.split("\n")
    if not lines or lines[0].strip() != "---":
        return text
    for index in range(1, len(lines)):
        if lines[index].strip() == "---":
            return "\n".join(lines[index + 1 :])
    return text  # unterminated frontmatter: compare the whole file


def check(root: Path) -> list[str]:
    errors: list[str] = []

    canonical_path = root / CANONICAL_SKILL
    if not canonical_path.is_file():
        errors.append(f"{CANONICAL_SKILL}: missing (canonical review checklist)")
    else:
        canonical = strip_frontmatter(read_text(canonical_path)).splitlines()
        for mirror in MIRROR_SKILLS:
            mirror_path = root / mirror
            if not mirror_path.is_file():
                errors.append(f"{mirror}: missing (copy of {CANONICAL_SKILL})")
                continue
            body = strip_frontmatter(read_text(mirror_path)).splitlines()
            if body != canonical:
                diff = "\n".join(
                    difflib.unified_diff(canonical, body, CANONICAL_SKILL, mirror, lineterm="", n=1)
                )
                errors.append(
                    f"{mirror}: body differs from {CANONICAL_SKILL} (frontmatter ignored); "
                    f"make the bodies identical:\n{diff}"
                )

    if not (root / AGENTS_MD).is_file():
        errors.append(f"{AGENTS_MD}: missing (canonical agent guidance)")
    claude_path = root / CLAUDE_MD
    if not claude_path.is_file():
        errors.append(f"{CLAUDE_MD}: missing")
    elif not any(line.strip() == IMPORT_LINE for line in read_text(claude_path).split("\n")):
        errors.append(f"{CLAUDE_MD}: no '{IMPORT_LINE}' line; it must import the shared rules from {AGENTS_MD}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"check-guidance-sync: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
