#!/usr/bin/env python3
"""
tools/check-utf8.py -- Fail when a text file is not valid UTF-8 (#1648), or holds double-encoded text (#2529).

Usage:
    python -I tools/check-utf8.py FILE...

Reads each file as bytes and decodes it strictly as UTF-8, one line at a time, so every bad line is
reported as `file:line: <reason>`. A UTF-16 or UTF-32 byte-order mark is reported as an error too: the
repository is UTF-8 only. A UTF-8 BOM is valid UTF-8 and is left to pre-commit's
`fix-byte-order-marker` hook, which removes it.

Double-encoded ("mojibake") text is valid UTF-8 too, so it is looked for separately: UTF-8 that was
read as Windows-1252 or Latin-1 and saved as UTF-8 again, such as an em dash turned into U+00E2 U+20AC
U+201D or into U+00E2 plus two C1 controls (#2529). A line is flagged when a run of characters maps
back, byte for byte, to a valid UTF-8 encoding of a non-ASCII character. Write such text correctly,
or as escaped bytes ("\\xE2\\x80\\x94") in a C++ literal.

Run by pre-commit on staged text and binary files (`types_or: [text, binary]`, #4333): a UTF-16 file
whose type identify reads from its content is tagged binary, and is exactly what this must catch. The
real binary assets are excluded there by extension. Exits 1 if any file failed, 0 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_WIDE_BOMS = (
    (b"\xff\xfe\x00\x00", "UTF-32 LE"),
    (b"\x00\x00\xfe\xff", "UTF-32 BE"),
    (b"\xff\xfe", "UTF-16 LE"),
    (b"\xfe\xff", "UTF-16 BE"),
)


def _single_byte_chars(byte: int) -> list[str]:
    """The characters `byte` reads as in Latin-1 and in Windows-1252 (the same one from A0 up)."""
    chars = [bytes([byte]).decode("latin-1")]
    try:
        chars.append(bytes([byte]).decode("cp1252"))
    except UnicodeDecodeError:
        pass  # 81, 8D, 8F, 90 and 9D are undefined in 1252
    return chars


# A UTF-8 lead byte (C2-F4) read as one character, then one to three continuation bytes (80-BF) read the
# same way: the shape a non-ASCII character takes once its UTF-8 bytes are decoded as 1252 or Latin-1.
_CHAR_TO_BYTE = {char: byte for byte in range(0x80, 0x100) for char in _single_byte_chars(byte)}
_LEADS = "".join(char for byte in range(0xC2, 0xF5) for char in _single_byte_chars(byte))
_CONTINUATIONS = "".join(char for byte in range(0x80, 0xC0) for char in _single_byte_chars(byte))
_MOJIBAKE = re.compile(f"[{re.escape(_LEADS)}][{re.escape(_CONTINUATIONS)}]{{1,3}}")


# Windows PowerShell 5.1 reads a script with no BOM as the ANSI code page, so a non-ASCII byte in a string can end
# it early and give parse errors instead of "run this in pwsh" (#4319). pre-commit strips BOMs, so a .ps1 with
# non-ASCII text must declare `#Requires -Version 7`, which 5.1 reports cleanly; the rest stay ASCII-only.
_REQUIRES_PWSH7 = re.compile(rb"(?im)^#Requires\s+-Version\s+(?:[7-9]|\d\d)(?:\.\d+)*\s*$")
_NON_ASCII = re.compile(rb"[\x80-\xff]")


def powershell_5_1_problem(path: str, data: bytes) -> list[str]:
    """One message when a .ps1 holds non-ASCII text but doesn't require PowerShell 7 (empty otherwise)."""
    if Path(path).suffix.lower() != ".ps1" or _REQUIRES_PWSH7.search(data):
        return []
    for line_number, line in enumerate(data.split(b"\n"), start=1):
        found = _NON_ASCII.search(line)
        if found:
            return [
                f"{path}:{line_number}:{found.start() + 1}: non-ASCII text in a .ps1 without `#Requires -Version 7`; "
                "Windows PowerShell 5.1 reads it as the ANSI code page and fails to parse instead of saying to use "
                "pwsh. Write it with [char]0x... or add the #Requires line (#4319)"
            ]
    return []


def find_mojibake(text: str) -> list[tuple[int, str, str]]:
    """Returns (column, garbled text, what it was meant to be) for each double-encoded character in `text`."""
    found: list[tuple[int, str, str]] = []
    for match in _MOJIBAKE.finditer(text):
        raw = bytes(_CHAR_TO_BYTE[char] for char in match.group())
        try:
            meant = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        found.append((match.start() + 1, match.group(), meant))
    return found


def check_file(path: str) -> list[str]:
    """Returns one message per problem in `path` (empty when the file is valid, singly encoded UTF-8)."""
    try:
        with open(path, "rb") as stream:
            data = stream.read()
    except OSError as error:
        return [f"{path}: cannot read: {error.strerror}"]

    for bom, name in _WIDE_BOMS:
        if data.startswith(bom):
            return [f"{path}:1: starts with a {name} byte-order mark; convert the file to UTF-8 without a BOM"]

    problems: list[str] = powershell_5_1_problem(path, data)
    for line_number, line in enumerate(data.split(b"\n"), start=1):
        try:
            text = line.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            bad = line[error.start : error.end].hex(" ")
            problems.append(
                f"{path}:{line_number}:{error.start + 1}: not valid UTF-8 (byte(s) {bad}: {error.reason})"
            )
            continue
        for column, garbled, meant in find_mojibake(text):
            problems.append(
                f"{path}:{line_number}:{column}: double-encoded text (bytes {garbled.encode('utf-8').hex(' ')} "
                f"are U+{ord(meant):04X} encoded twice; write {meant.encode('utf-8').hex(' ')})"
            )
    return problems


def main(argv: list[str]) -> int:
    failed = False
    for path in argv:
        for problem in check_file(path):
            print(problem)
            failed = True
    if failed:
        print(
            "Convert these files to UTF-8 without a BOM, and re-type double-encoded text "
            "(CONTRIBUTING.md, 'Text encoding and locale')."
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
