#!/usr/bin/env python3
"""
tools/cache-prune-stale.py -- Pick the superseded Actions cache entries that cache-prune.yml deletes (#1583).

Usage:
    gh api --paginate ... --jq '... | [.id, .key, .created_at, .size_in_bytes] | @tsv' \\
        | python3 -I tools/cache-prune-stale.py > stale.tsv

Reads one cache entry per line on stdin, `id <TAB> key <TAB> created_at <TAB> size_in_bytes`, and
writes every entry that a newer entry of the same family supersedes, one per line, as
`family <TAB> created_at <TAB> id <TAB> size_in_bytes <TAB> key`, ordered by family and then newest
first.

A family is a key with its per-save part removed:

  - ccache-<key>-<ISO timestamp>         (hendrikmuhs/ccache-action, a new entry every main run)
  - <OS>-fetchcontent-<hash>             (.github/actions/fetchcontent-cache)
  - <OS>[-<ARCH>]-cmake-checks-<preset>-<hash>-v<N>  (.github/actions/cmake-check-cache)
  - <OS>-tidy-results-<run id>-<attempt> (.github/actions/tidy-result-cache, #1785)
  - codeql-trap-<N>-<CLI version>-<language>-<commit SHA>  (github/codeql-action's TRAP cache, #3101)

Restores only ever use the newest entry of a family, so every older one is stale. Anything else (the
LLVM, IWYU and setup-python caches) has a single live key and is never picked.

The CodeQL TRAP cache is saved once per analyzed commit of the default branch (about 640 MB each, and
main analyzes 20-30 commits on a busy day), which main's own runs never read back ("Analyzing default
branch. Skipping downloading of TRAP caches"); a pull request asks for its base commit's entry and falls
back to the newest by prefix ("Cache hit for restore-key"). Nothing deleted the older ones, so they
kept the repository over its cache limit, and GitHub's least-recently-used eviction then removed the
small entries nothing had read for a few hours: main's clang-tidy result caches, saved once a night
(#3101).

A family must name everything that makes two entries non-interchangeable, or the newest one evicts
the other's live entry. x64 and ARM64 Windows runners once both saved
Windows-cmake-checks-win-debug-<hash>-v1 (the architecture reached only the hash), so each prune
deleted the other architecture's entry (#2528); the cmake-checks key now carries a non-x64 runner's
architecture after the OS, as the ccache keys already did (#2214).
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable

# Applied in order; the first whose `match` finds the key decides its family.
FAMILY_RULES: tuple[tuple[re.Pattern[str], re.Pattern[str], str], ...] = (
    (re.compile(r"^ccache-"), re.compile(r"-\d{4}-\d{2}-\d{2}T[\d:.]*Z$"), ""),
    (re.compile(r"-fetchcontent-"), re.compile(r"-fetchcontent-.*$"), "-fetchcontent"),
    (re.compile(r"-cmake-checks-"), re.compile(r"-[0-9a-f]+-v\d+$"), ""),
    (re.compile(r"-tidy-results-"), re.compile(r"-\d+-\d+$"), ""),
    (re.compile(r"^codeql-trap-"), re.compile(r"-[0-9a-f]{40}$"), ""),
)


def family(key: str) -> str | None:
    """The key's family, or None when the key isn't one cache-prune manages."""
    for match, per_save, replacement in FAMILY_RULES:
        if match.search(key):
            name = per_save.sub(replacement, key, count=1)
            # Not a timestamped/hashed key after all: leave it alone.
            return name if name != key else None
    return None


def stale_entries(lines: Iterable[str]) -> list[tuple[str, str, str, str, str]]:
    """(family, created_at, id, bytes, key) for every entry a newer one of its family supersedes."""
    entries: list[tuple[str, str, str, str, str]] = []
    for line in lines:
        line = line.rstrip("\r\n")
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"expected id, key, created_at and size_in_bytes, got {line!r}")
        cache_id, key, created, size = fields
        name = family(key)
        if name is not None:
            entries.append((name, created, cache_id, size, key))

    # Newest first within a family (ISO 8601 UTC timestamps sort as text); a tie goes to the higher,
    # later-assigned id.
    entries.sort(key=lambda e: (e[1], int(e[2]) if e[2].isdigit() else 0), reverse=True)
    entries.sort(key=lambda e: e[0])
    stale: list[tuple[str, str, str, str, str]] = []
    previous: str | None = None
    for entry in entries:
        if entry[0] == previous:
            stale.append(entry)
        previous = entry[0]
    return stale


def main() -> int:
    try:
        stale = stale_entries(sys.stdin)
    except ValueError as error:
        print(f"cache-prune-stale: {error}", file=sys.stderr)
        return 1
    for entry in stale:
        print("\t".join(entry))
    return 0


if __name__ == "__main__":
    sys.exit(main())
