#!/usr/bin/env python3
"""
tools/check-version-ahead.py -- Fail when a branch's version is below a release already published (#3371).

Usage:
    git ls-remote --tags --refs origin 'v*' | python -I tools/check-version-ahead.py --branch main
    python -I tools/check-version-ahead.py --branch release/0.10 --tags-file tags.txt
    python -I tools/check-version-ahead.py --self-test

`project(TaskSmack VERSION X.Y.Z)` in CMakeLists.txt is the only version. Between releases main must
read the *next* minor (it's bumped right after each `release/X.Y` cut, docs/dev/release-process.md),
so its builds show `X.Y.Z-dev` and order above everything published. This 0.10 cycle main read 0.9.0
for weeks, below the shipped v0.9.1, and a main build was offered "v0.9.1 available".

The tags are release tags, `vX.Y.Z` with nothing after the numbers, read from `git ls-remote --tags`
output (`<sha>\\trefs/tags/vX.Y.Z`; one bare tag name per line works too) on stdin or --tags-file.
Only the maintainer pushes them, and only for a release, so the highest is the newest version shipped.

The rule, by the branch the version is on (a pull request's base branch, or the pushed branch):

  - `release/X.Y`: PROJECT_VERSION must be X.Y.something. Compared with the highest `vX.Y.*` tag only
    (a newer minor shipping later doesn't make an old line's branch wrong): below it fails; equal
    passes, since that's the branch between a patch release and the next patch's bump; above passes
    (the next patch's bump, or X.Y.0 before its tag).
  - anything else (main): compared with the highest tag of all. Below it fails; equal passes with a
    notice, since that's only right in the window between pushing a minor's tag and main's bump to the
    next minor; above passes.

Exits 1 on a failure (or input it can't read), 0 otherwise. Prints GitHub Actions `::error::` and
`::notice::` annotations. Needs nothing beyond the standard library, and no token.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PROJECT_LINE = re.compile(r"^project\(TaskSmack VERSION (\S+)", re.MULTILINE)
VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RELEASE_TAG = re.compile(r"(?:^|\s|refs/tags/)v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))$")
RELEASE_BRANCH = re.compile(r"^release/(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")

Version = tuple[int, int, int]


def parse_version(text: str) -> Version | None:
    match = VERSION.match(text)
    if match is None:
        return None
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def text_of(version: Version) -> str:
    return ".".join(str(part) for part in version)


def release_tags(lines: list[str]) -> list[Version]:
    """The release versions among `git ls-remote --tags` lines; pre-release tags and others are skipped."""
    found: list[Version] = []
    for line in lines:
        match = RELEASE_TAG.search(line.strip())
        if match is not None:
            version = parse_version(match.group(1))
            if version is not None:
                found.append(version)
    return found


def check(project: Version, branch: str, tags: list[Version]) -> tuple[str, str]:
    """('fail' | 'notice' | 'pass', message) for @p project on @p branch, given the published @p tags."""
    release = RELEASE_BRANCH.match(branch)
    if release is not None:
        line = (int(release.group(1)), int(release.group(2)))
        if project[:2] != line:
            return ("fail", f"{branch} reads {text_of(project)}; a release/{line[0]}.{line[1]} branch must read "
                    f"{line[0]}.{line[1]}.<patch>.")
        scoped = [tag for tag in tags if tag[:2] == line]
        if not scoped:
            return ("pass", f"{branch} reads {text_of(project)}; no v{line[0]}.{line[1]}.* tag is published yet.")
        highest = max(scoped)
        if project < highest:
            return ("fail", f"{branch} reads {text_of(project)}, below v{text_of(highest)}, which is already "
                    f"published. Bump the patch in CMakeLists.txt's project(... VERSION ...).")
        if project == highest:
            return ("pass", f"{branch} reads {text_of(project)}, its latest release; the next patch's bump is "
                    f"pending, as it should be between patch releases.")
        return ("pass", f"{branch} reads {text_of(project)}, above its latest release v{text_of(highest)}.")

    if not tags:
        return ("pass", f"{branch} reads {text_of(project)}; no release tag is published.")
    highest = max(tags)
    if project < highest:
        return ("fail", f"{branch} reads {text_of(project)}, below v{text_of(highest)}, which is already published, "
                f"so its builds would show and compare as an older release. Bump CMakeLists.txt's "
                f"project(... VERSION ...) to the next minor ({highest[0]}.{highest[1] + 1}.0), as the release "
                f"process does right after cutting release/{highest[0]}.{highest[1]}.")
    if project == highest:
        return ("notice", f"{branch} reads {text_of(project)}, the same as the latest release v{text_of(highest)}. "
                f"That's right only until main is bumped to the next minor ({highest[0]}.{highest[1] + 1}.0) "
                f"after the release/{highest[0]}.{highest[1]} cut.")
    return ("pass", f"{branch} reads {text_of(project)}, above the latest release v{text_of(highest)}.")


def self_test() -> int:
    tags = release_tags([
        "aaa\trefs/tags/v0.8.0",
        "bbb\trefs/tags/v0.9.0",
        "ccc\trefs/tags/v0.9.1",
        "ddd\trefs/tags/v0.10.0-rc.1",  # a pre-release tag isn't a release
        "eee\trefs/tags/v0.10.0^{}",     # a peeled line (--refs leaves them out) is skipped, not misread
        "fff\trefs/tags/nightly",
        "v0.7.1",
    ])
    assert sorted(tags) == [(0, 7, 1), (0, 8, 0), (0, 9, 0), (0, 9, 1)], tags
    cases = [
        # The 0.10 cycle's bug: main read 0.9.0 after v0.9.1 shipped.
        ((0, 9, 0), "main", tags, "fail"),
        ((0, 9, 1), "main", tags, "notice"),
        ((0, 10, 0), "main", tags, "pass"),
        ((0, 10, 0), "main", [], "pass"),
        # After v0.10.0 ships: main is 0.11.0; release/0.10 between patches; release/0.9 stays valid.
        ((0, 11, 0), "main", tags + [(0, 10, 0)], "pass"),
        ((0, 10, 0), "main", tags + [(0, 10, 0)], "notice"),
        ((0, 10, 0), "release/0.10", tags + [(0, 10, 0)], "pass"),
        ((0, 10, 1), "release/0.10", tags + [(0, 10, 0)], "pass"),
        ((0, 10, 0), "release/0.10", tags + [(0, 10, 0), (0, 10, 1)], "fail"),
        ((0, 10, 0), "release/0.10", tags, "pass"),  # cut, not yet tagged
        ((0, 9, 1), "release/0.9", tags + [(0, 10, 0)], "pass"),
        ((0, 9, 0), "release/0.9", tags, "fail"),
        ((0, 11, 0), "release/0.10", tags, "fail"),  # a release branch reading another line
        # Numbers, not text: 0.10.0 is above 0.9.1.
        ((0, 10, 0), "main", [(0, 9, 1)], "pass"),
        ((0, 9, 10), "main", [(0, 10, 0)], "fail"),
    ]
    for project, branch, case_tags, expected in cases:
        outcome, message = check(project, branch, case_tags)
        assert outcome == expected, (project, branch, case_tags, outcome, message)
    assert parse_version("0.10.0") == (0, 10, 0)
    for bad in ("0.10", "0.10.0-dev", "v0.10.0", "00.1.0", "0.1.0 "):
        assert parse_version(bad) is None, bad
    print(f"check-version-ahead self-test: {len(cases)} cases passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--branch", help="the branch the version is on: a PR's base branch, or the pushed branch")
    parser.add_argument("--tags-file", type=Path, help="`git ls-remote --tags` output (default: stdin)")
    parser.add_argument("--cmakelists", type=Path, default=Path("CMakeLists.txt"))
    parser.add_argument("--self-test", action="store_true", help="run the rule's own tests and exit")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.branch:
        parser.error("--branch is required")

    match = PROJECT_LINE.search(args.cmakelists.read_text(encoding="utf-8"))
    project = parse_version(match.group(1)) if match is not None else None
    if project is None:
        print(f"::error file={args.cmakelists}::Could not read a plain X.Y.Z from project(TaskSmack VERSION ...).")
        return 1

    text = args.tags_file.read_text(encoding="utf-8") if args.tags_file else sys.stdin.read()
    tags = release_tags(text.splitlines())

    outcome, message = check(project, args.branch, tags)
    if outcome == "fail":
        print(f"::error file={args.cmakelists}::{message}")
        return 1
    if outcome == "notice":
        print(f"::notice file={args.cmakelists}::{message}")
    else:
        print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
