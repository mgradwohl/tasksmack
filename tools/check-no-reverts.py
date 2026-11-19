#!/usr/bin/env python3
"""
tools/check-no-reverts.py -- fail a change whose diff undoes a recent merge (#3986, after #3972).

Usage:
    check-no-reverts.py --base BASE_SHA --head HEAD_SHA [--window 60] [--min-lines 6] [--min-chars 25]
        [--labels LABELS] [--body-file BODY.md] [--summary SUMMARY.md] [--advisory]

Two squash merges reverted six merged PRs (#3972): their branches held stale copies of files that other PRs had
changed, and CI passed because the reverted state builds and tests on its own. Neither CI nor a merge queue sees
that as wrong; this check does. The rule, as reviewed on #3986:

- The change is HEAD against its merge base with BASE. It can only remove lines that exist at the merge base, so
  the window is the last --window first-parent commits ending AT the merge base.
- Each removed line is attributed with `git blame -w` over that window alone (a line older than the window is a
  boundary line and never counts), not by matching text, which braces and repeated #includes would defeat.
- A removed line the change adds back anywhere, in any file, moved and doesn't count; nor does reformatting
  (text is compared with its whitespace collapsed). Only lines longer than --min-chars count.
- --min-lines or more removed lines blamed on one window commit is a finding. A deleted file is the same rule.
- Lockfiles and pin files are exempt by path (EXEMPT), never by author: a bot change that removes real code is
  what this is for.

A finding is cleared by the ci:revert-ok label (in --labels, comma-separated), or, for that commit alone, by a
change description (--body-file) that names the PR the commit merged (#N in its subject). Every finding, cleared
or not, and every line counted as moved go to --summary, so false positives can be counted (#3986's advisory
week). Findings are GitHub annotations: warnings with --advisory, else errors.

Exit codes: 0 clean, cleared, or --advisory; 1 an uncleared finding; 2 usage error or unusable history (a merge
base or window outside the fetched history).
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

EXEMPT = ("vcpkg*.json", "*.lock", "package-lock.json", "docs/cdn-integrity.json")
OVERRIDE_LABEL = "ci:revert-ok"
HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@")
PR_REF = re.compile(r"#(\d+)\b")
# A squash merge's subject ends with the PR it merged: "... (#3952)". A backport's carries its own too.
SUBJECT_PR = re.compile(r"\(#(\d+)\)\s*$")
MOVED_LIST_MAX = 50


class HistoryError(Exception):
    """The fetched history doesn't hold what the check needs: exit 2."""


def git(repo: Path, *args: str) -> str:
    # UTF-8, not the locale's code page (on Windows, "café" printed as its UTF-8 bytes read in the code page), and core.quotePath=false so a non-ASCII path
    # is itself in the diff headers and listings, not an octal-escaped quoted name (#4331).
    result = subprocess.run(
        ["git", "-c", "core.quotePath=false", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise HistoryError(f"git {' '.join(args[:3])}...: {result.stderr.strip() or result.returncode}")
    return result.stdout


def norm(text: str) -> str:
    return " ".join(text.split())


def exempt(path: str) -> bool:
    name = PurePosixPath(path).name
    return any(fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(name, pattern) for pattern in EXEMPT)


def header_path(line: str, side: str) -> str | None:
    r"""The path on a "--- a/x" or "+++ b/x" diff header, or None for /dev/null. git still quotes a name holding a tab,
    newline, quote or backslash even with core.quotePath=false ("--- \"a/x\ty\""): unquote that too (#4331)."""
    rest = line[4:]
    if rest.startswith('"') and rest.endswith('"'):
        rest = unquote_c_style(rest[1:-1])
    prefix = side + "/"
    return rest[len(prefix) :] if rest.startswith(prefix) else None


def unquote_c_style(text: str) -> str:
    r"""git's C-style quoting: \\, \", \t, \n and \ooo octal bytes, the bytes then decoded as UTF-8."""
    out = bytearray()
    i = 0
    escapes = {"\\": b"\\", '"': b'"', "t": b"\t", "n": b"\n", "a": b"\a", "b": b"\b", "f": b"\f", "r": b"\r", "v": b"\v"}
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if len(digits := text[i + 1 : i + 4]) == 3 and all(c in "01234567" for c in digits):
                out.append(int(digits, 8))
                i += 4
                continue
            out += escapes.get(nxt, nxt.encode())
            i += 2
            continue
        out += ch.encode()
        i += 1
    return out.decode("utf-8", errors="replace")


def parse_diff(diff: str) -> tuple[dict[str, list[tuple[int, int]]], dict[str, str]]:
    """The old-side line ranges each file's hunks remove, and every line the diff adds (normalised -> its file)."""
    ranges: dict[str, list[tuple[int, int]]] = {}
    added: dict[str, str] = {}
    old = new = None
    for line in diff.splitlines():
        if line.startswith("--- "):
            old = header_path(line, "a")
        elif line.startswith("+++ "):
            new = header_path(line, "b")
        elif match := HUNK.match(line):
            start, count = int(match.group(1)), int(match.group(2) or "1")
            if old is not None and count > 0:
                ranges.setdefault(old, []).append((start, count))
        elif line.startswith("+") and new is not None:
            added.setdefault(norm(line[1:]), new)
    return ranges, added


def blame(repo: Path, revisions: list[str], path: str, start: int, count: int) -> list[tuple[str | None, str]]:
    """(commit, text) for each line of @p path in [start, start + count), over @p revisions (the window); commit is
    None for a line older than the window (a blame boundary)."""
    out = git(repo, "blame", "-w", "--line-porcelain", "-L", f"{start},+{count}", *revisions, "--", path)
    lines: list[tuple[str | None, str]] = []
    commit: str | None = None
    boundary = False
    for line in out.splitlines():
        if re.match(r"^[0-9a-f]{40} \d+ \d+", line):
            commit, boundary = line[:40], False
        elif line == "boundary":
            boundary = True
        elif line.startswith("\t"):
            lines.append((None if boundary else commit, line[1:]))
    return lines


@dataclass
class Finding:
    commit: str
    subject: str
    lines: int
    files: set[str] = field(default_factory=set)
    cleared_by: str | None = None


@dataclass
class Result:
    merge_base: str
    window: list[str]
    findings: list[Finding]
    moved: list[tuple[str, str, str]]  # (removed from, added in, text)


def check(repo: Path, base: str, head: str, window_size: int, min_lines: int, min_chars: int) -> Result:
    merge_base = git(repo, "merge-base", base, head).strip()
    window = git(repo, "rev-list", "--first-parent", f"--max-count={window_size}", merge_base).split()
    if not window:
        raise HistoryError("no commits before the merge base")
    oldest = window[-1]
    # Blame over the window alone: from the oldest window commit's parent. A window that reaches the root has
    # no parent there, so the whole history is the window, and --root counts the root's own lines.
    has_parent = bool(git(repo, "rev-list", "--parents", "-n", "1", oldest).split()[1:])
    revisions = [f"{oldest}^..{merge_base}"] if has_parent else ["--root", merge_base]
    ranges, added = parse_diff(git(repo, "diff", "-U0", "--no-renames", merge_base, head))
    in_window = set(window)
    counts: dict[str, Finding] = {}
    moved: list[tuple[str, str, str]] = []
    for path, hunks in ranges.items():
        if exempt(path):
            continue
        for start, count in hunks:
            lines = blame(repo, revisions, path, start, count)
            for commit, text in lines:
                key = norm(text)
                if len(key) <= min_chars or commit is None or commit not in in_window:
                    continue
                if key in added:
                    moved.append((path, added[key], key))
                    continue
                finding = counts.setdefault(commit, Finding(commit, "", 0))
                finding.lines += 1
                finding.files.add(path)
    findings = [f for f in counts.values() if f.lines >= min_lines]
    for finding in findings:
        finding.subject = git(repo, "log", "-1", "--format=%s", finding.commit).strip()
    findings.sort(key=lambda f: window.index(f.commit))
    return Result(merge_base, window, findings, moved)


def clear(findings: list[Finding], labels: set[str], body: str) -> None:
    named = {int(n) for n in PR_REF.findall(body)}
    for finding in findings:
        merged = SUBJECT_PR.search(finding.subject)
        if OVERRIDE_LABEL in labels:
            finding.cleared_by = f"the {OVERRIDE_LABEL} label"
        elif merged and int(merged.group(1)) in named:
            finding.cleared_by = f"the description names #{merged.group(1)}"


def summary(result: Result, advisory: bool) -> str:
    out = ["## Revert guard (#3986)", ""]
    out.append(f"Merge base `{result.merge_base[:12]}`, window of {len(result.window)} first-parent commits ending there.")
    out.append("")
    if not result.findings:
        out.append("No removed lines attributed to a recent merge.")
    else:
        out.append("| Commit | Merged | Lines removed | Files | Status |")
        out.append("|---|---|---|---|---|")
        for f in result.findings:
            status = f"cleared: {f.cleared_by}" if f.cleared_by else ("advisory" if advisory else "**fails**")
            files = ", ".join(f"`{p}`" for p in sorted(f.files))
            out.append(f"| `{f.commit[:8]}` | {f.subject.replace('|', '/')} | {f.lines} | {files} | {status} |")
    if result.moved:
        out += ["", f"<details><summary>{len(result.moved)} removed line(s) counted as moved</summary>", "", "```"]
        out += [f"{src} -> {dst}: {text[:100]}" for src, dst, text in result.moved[:MOVED_LIST_MAX]]
        if len(result.moved) > MOVED_LIST_MAX:
            out.append(f"... and {len(result.moved) - MOVED_LIST_MAX} more")
        out += ["```", "</details>"]
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--base", required=True, help="the base branch's commit (a PR's base sha, a merge group's base_sha)")
    parser.add_argument("--head", required=True, help="the change's commit")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--window", type=int, default=60, help="first-parent commits ending at the merge base")
    parser.add_argument("--min-lines", type=int, default=6)
    parser.add_argument("--min-chars", type=int, default=25)
    parser.add_argument("--labels", default="", help="the change's labels, comma-separated")
    parser.add_argument("--body-file", type=Path, default=None, help="the change's description")
    parser.add_argument("--summary", type=Path, default=None, help="append the Markdown report here")
    parser.add_argument("--advisory", action="store_true", help="report findings as warnings and pass")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return 0 if error.code == 0 else 2
    try:
        result = check(args.repo, args.base, args.head, args.window, args.min_lines, args.min_chars)
    except HistoryError as error:
        print(f"::warning title=Revert guard::not checked: {error}" if args.advisory else f"ERROR: {error}", file=sys.stderr)
        return 0 if args.advisory else 2
    body = args.body_file.read_text(encoding="utf-8", errors="replace") if args.body_file and args.body_file.is_file() else ""
    clear(result.findings, {label.strip() for label in args.labels.split(",") if label.strip()}, body)
    report = summary(result, args.advisory)
    print(report)
    if args.summary is not None:
        with open(args.summary, "a", encoding="utf-8") as stream:
            stream.write(report)
    failing = [f for f in result.findings if f.cleared_by is None]
    level = "warning" if args.advisory else "error"
    for f in failing:
        print(f"::{level} title=Revert guard::This change removes {f.lines} line(s) that {f.commit[:8]} ({f.subject}) "
              f"added, in {', '.join(sorted(f.files))}. If that's deliberate, add the {OVERRIDE_LABEL} label or name "
              "the PR in the description.")
    return 1 if failing and not args.advisory else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
