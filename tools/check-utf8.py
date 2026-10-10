#!/usr/bin/env python3
"""
tools/check-utf8.py -- Fail when a text file is not valid UTF-8 (#1648).

Usage:
    python -I tools/check-utf8.py FILE...

Reads each file as bytes and decodes it strictly as UTF-8, one line at a time, so every bad line is
reported as `file:line: <reason>`. A UTF-16 or UTF-32 byte-order mark is reported as an error too: the
repository is UTF-8 only. A UTF-8 BOM is valid UTF-8 and is left to pre-commit's
`fix-byte-order-marker` hook, which removes it.

Run by pre-commit on staged text files (`types: [text]`); binary assets are excluded there, matching
.gitattributes/.editorconfig. Exits 1 if any file failed, 0 otherwise.
"""

from __future__ import annotations

import sys

_WIDE_BOMS = (
    (b"\xff\xfe\x00\x00", "UTF-32 LE"),
    (b"\x00\x00\xfe\xff", "UTF-32 BE"),
    (b"\xff\xfe", "UTF-16 LE"),
    (b"\xfe\xff", "UTF-16 BE"),
)


def check_file(path: str) -> list[str]:
    """Returns one message per problem in `path` (empty when the file is valid UTF-8)."""
    try:
        with open(path, "rb") as stream:
            data = stream.read()
    except OSError as error:
        return [f"{path}: cannot read: {error.strerror}"]

    for bom, name in _WIDE_BOMS:
        if data.startswith(bom):
            return [f"{path}:1: starts with a {name} byte-order mark; convert the file to UTF-8 without a BOM"]

    problems: list[str] = []
    for line_number, line in enumerate(data.split(b"\n"), start=1):
        try:
            line.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            bad = line[error.start : error.end].hex(" ")
            problems.append(
                f"{path}:{line_number}:{error.start + 1}: not valid UTF-8 (byte(s) {bad}: {error.reason})"
            )
    return problems


def main(argv: list[str]) -> int:
    failed = False
    for path in argv:
        for problem in check_file(path):
            print(problem)
            failed = True
    if failed:
        print("Convert these files to UTF-8 without a BOM (CONTRIBUTING.md, 'Text encoding and locale').")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
