#!/usr/bin/env python3
"""
tools/compose-release-notes.py -- Write a GitHub release's notes over release.yml's placeholder (#3144).

Usage:
    tools/compose-release-notes.py --version X.Y.Z --template <download-guide.md>
        --changelog <git-cliff section> --release-json <gh release view --json body,assets>
        --output <notes.md>

release.yml publishes each release with a placeholder body: "Release notes follow.", then a
"## Verifying downloads" block. changelog.yml's release-notes job runs this once the release is
published. When the body still starts with the placeholder, it writes to --output:

  1. "# TaskSmack vX.Y.Z";
  2. the "Which file should I download?" guide, from --template with {{VERSION}} and
     {{VERIFY_FILES}} filled in. The second names `SHA256SUMS` and `SHA256SUMS.bundle` when the
     release has them, and the per-file `.bundle` files otherwise (v0.9.x);
  3. "## Changes" and git-cliff's [X.Y.Z] section, without its own "## [X.Y.Z] - date" line and
     with every heading one level deeper;
  4. the body's own "## Verifying downloads" block, from that heading to the end, so it always
     matches what release.yml wrote.

When the body doesn't start with the placeholder, someone has already written the notes: it says
"release notes already set; left as is" and writes nothing, so the caller edits nothing.

GitHub refuses a release body over 125,000 characters (#2801). Notes over MAX_BYTES (counted in
UTF-8 bytes, which is never fewer than characters) keep as many whole changelog lines as fit and
end the list with a link to the full section in CHANGELOG.md.

Standard library only, like the tools/check-*.py checkers. Exit status: 0 when the notes were
written or left as they were, 2 on bad input (a missing file, a section for another version, a
placeholder left unfilled in the template).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

PLACEHOLDER = "Release notes follow."
VERIFY_HEADING = "## Verifying downloads"
# GitHub's limit is 125,000 characters; the rest is headroom.
MAX_BYTES = 120_000
CHANGELOG_URL = "https://github.com/mgradwohl/tasksmack/blob/main/CHANGELOG.md"
LEFT_AS_IS = "release notes already set; left as is"

_LEADING_COMMENT = re.compile(r"\A\s*<!--.*?-->\s*", re.DOTALL)
_TEMPLATE_FIELD = re.compile(r"\{\{\s*([A-Z_]+)\s*\}\}")
_SECTION_HEADER = re.compile(r"^## \[(?P<version>[^\]]+)\](?P<rest>.*)$")
_HEADING = re.compile(r"^(#{1,5}) ")
_FENCE = re.compile(r"^\s*(```|~~~)")


class CompositionError(ValueError):
    """Input the notes can't be composed from."""


def _lines(text: str) -> list[str]:
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def is_placeholder(body: str) -> bool:
    """True while the body is still release.yml's placeholder (it may lead with blank lines)."""
    return body.replace("\r\n", "\n").lstrip().startswith(PLACEHOLDER)


def verification_block(body: str) -> str:
    """The body's "## Verifying downloads" heading and everything after it; empty when it has none."""
    lines = _lines(body)
    for index, line in enumerate(lines):
        if line.rstrip() == VERIFY_HEADING:
            return "\n".join(lines[index:]).strip()
    return ""


def verify_files(assets: list[str]) -> str:
    """The guide's name for the files that only check a download, by what the release carries."""
    if "SHA256SUMS" in assets and "SHA256SUMS.bundle" in assets:
        return "`SHA256SUMS`, `SHA256SUMS.bundle`"
    return "The `.bundle` files"


def render_guide(template: str, version: str, assets: list[str]) -> str:
    """The download guide for `version`: the template's leading comment dropped, its fields filled."""
    values = {"VERSION": version, "VERIFY_FILES": verify_files(assets)}
    text = _LEADING_COMMENT.sub("", template, count=1)

    def fill(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in values:
            raise CompositionError(f"the template has an unknown field {{{{{name}}}}}")
        return values[name]

    return _TEMPLATE_FIELD.sub(fill, text).strip()


def _anchor(header: str) -> str:
    """GitHub's anchor for a Markdown heading's text: "[0.9.1] - 2026-10-27" -> "091---2026-10-27"."""
    slug = re.sub(r"[^\w\- ]", "", header.strip().lower())
    return slug.replace(" ", "-")


def changelog_lines(section: str, version: str) -> tuple[list[str], str]:
    """git-cliff's section for `version` as lines, with everything up to and including its
    "## [X.Y.Z] - date" line dropped and every heading demoted one level, and that line's anchor in
    CHANGELOG.md. Anything git-cliff wrote before the section (its file header) goes too."""
    lines = _lines(section)
    for start, line in enumerate(lines):
        match = _SECTION_HEADER.match(line)
        if match is None:
            continue
        if match.group("version") != version:
            raise CompositionError(f"the changelog section is for [{match.group('version')}], not [{version}]")
        anchor = _anchor(line[len("## ") :])
        body: list[str] = []
        fenced = False
        for text in lines[start + 1 :]:
            if _FENCE.match(text):
                fenced = not fenced
            elif not fenced and _HEADING.match(text):
                text = "#" + text
            body.append(text)
        while body and not body[0].strip():
            body.pop(0)
        while body and not body[-1].strip():
            body.pop()
        return body, anchor
    raise CompositionError(f"the changelog has no '## [{version}]' section header")


def _size(text: str) -> int:
    return len(text.encode("utf-8"))


def compose(
    version: str,
    template: str,
    section: str,
    body: str,
    assets: list[str],
    limit: int = MAX_BYTES,
) -> tuple[str, list[str]]:
    """The release notes, and what to log about them (a cut changelog, a missing block)."""
    log: list[str] = []
    title = f"# TaskSmack v{version}"
    guide = render_guide(template, version, assets)
    changes, anchor = changelog_lines(section, version)
    verify = verification_block(body)
    if not verify:
        log.append(f"warning: the release body has no '{VERIFY_HEADING}' block to keep")
    link = f"{CHANGELOG_URL}#{anchor}"
    if not changes:
        changes = [f"See [CHANGELOG.md]({link})."]

    def assemble(change_lines: list[str]) -> str:
        parts = [title, guide, "## Changes", "\n".join(change_lines)]
        if verify:
            parts.append(verify)
        return "\n\n".join(parts) + "\n"

    notes = assemble(changes)
    if _size(notes) <= limit:
        return notes, log

    cut_note = f"*The list is cut short to fit on this page. The full list is in [CHANGELOG.md]({link}).*"
    room = limit - _size(assemble(["", cut_note]))
    kept: list[str] = []
    used = 0
    for line in changes:
        cost = _size(line) + 1  # the line and its newline
        if used + cost > room:
            break
        kept.append(line)
        used += cost
    # Don't end on a heading with nothing under it, or on blank lines.
    while kept and (not kept[-1].strip() or _HEADING.match(kept[-1])):
        kept.pop()
    notes = assemble([*kept, "", cut_note] if kept else [cut_note])
    if _size(notes) > limit:
        raise CompositionError(f"the notes are {_size(notes)} bytes even without the changelog (limit {limit})")
    log.append(
        f"the changelog was cut to {len(kept)} of {len(changes)} lines to keep the notes under {limit} bytes; "
        f"they link to CHANGELOG.md for the rest"
    )
    return notes, log


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0].strip())
    parser.add_argument("--version", required=True, help="the release's version, X.Y.Z (no 'v')")
    parser.add_argument("--template", required=True, type=Path, help="the download guide template")
    parser.add_argument("--changelog", required=True, type=Path, help="git-cliff's section for the release")
    parser.add_argument(
        "--release-json", required=True, type=Path, help="`gh release view <tag> --json body,assets` output"
    )
    parser.add_argument("--output", required=True, type=Path, help="where to write the notes")
    parser.add_argument("--limit", type=int, default=MAX_BYTES, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    try:
        release = json.loads(args.release_json.read_text(encoding="utf-8"))
        body = release.get("body") or ""
        if not is_placeholder(body):
            print(LEFT_AS_IS)
            return 0
        assets = [asset["name"] for asset in release.get("assets") or []]
        notes, log = compose(
            args.version,
            args.template.read_text(encoding="utf-8"),
            args.changelog.read_text(encoding="utf-8"),
            body,
            assets,
            args.limit,
        )
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"compose-release-notes: {error}", file=sys.stderr)
        return 2

    args.output.write_text(notes, encoding="utf-8", newline="\n")
    for message in log:
        print(message)
    print(f"Wrote the v{args.version} release notes ({_size(notes)} bytes) to {args.output}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
