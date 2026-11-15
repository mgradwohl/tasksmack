#!/usr/bin/env python3
"""
tools/check-dll-loads.py -- Fail when src/ loads a DLL without restricting the search path (#2530).

Usage:
    python -I tools/check-dll-loads.py [--root DIR]

LoadLibraryW(L"pdh.dll") searches the application directory before System32, so a DLL of the same
name placed next to a portable TaskSmack.exe is loaded instead (and its DllMain runs, elevated if
TaskSmack is). So:

  - LoadLibrary / LoadLibraryA / LoadLibraryW are rejected outright, and
  - LoadLibraryEx / LoadLibraryExA / LoadLibraryExW must pass a LOAD_LIBRARY_SEARCH_* flag (in
    practice LOAD_LIBRARY_SEARCH_SYSTEM32), since flags of 0 search like the bare call.
  - Each Windows entry point (ENTRY_POINTS: TaskSmack.exe's WinMain and TaskSmackHelper.exe's
    wWinMain, the images that can run elevated, #3897) must call SetDefaultDllDirectories with
    LOAD_LIBRARY_SEARCH_SYSTEM32 as its first statement (namespace aliases aside), and never with
    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS or LOAD_LIBRARY_SEARCH_USER_DIRS. That restricts what third-party
    code (SDL, the GL loaders) and the DLLs it loads find by name; the linker's
    /DEPENDENTLOADFLAG:0x800 covers the imports (cmake/CompilerOptions.cmake).

Comments are ignored. This is a text scan of the C++ sources under src/, not a parse, so it needs
nothing beyond the standard library.

Run by pre-commit (local hook `check-dll-loads`) and by ctest (CheckDllLoadsScript). `--root` points
at another tree, for testing. Exits 1 on any failure, 0 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SOURCE_DIR = Path("src")
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp")

# String and character literals are matched so a "//" inside one isn't taken for a comment.
TOKENS = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.DOTALL)
LOAD_CALL = re.compile(r"\bLoadLibrary(Ex)?[AW]?\s*\(")
SEARCH_FLAG = "LOAD_LIBRARY_SEARCH_"

# The entry points that must restrict the default DLL directories first (#3897): file, function name.
ENTRY_POINTS = (
    (Path("src/main.cpp"), "WinMain"),
    (Path("src/Platform/Windows/HelperMain.cpp"), "wWinMain"),
)
SYSTEM32_FLAG = "LOAD_LIBRARY_SEARCH_SYSTEM32"
WIDE_FLAGS = ("LOAD_LIBRARY_SEARCH_DEFAULT_DIRS", "LOAD_LIBRARY_SEARCH_USER_DIRS")
SET_DEFAULT_DIRS = re.compile(r"SetDefaultDllDirectories\s*\(")
NAMESPACE_ALIAS = re.compile(r"namespace\s+\w+\s*=[^;]*;")
LEADING_SPACE = re.compile(r"\s*")


def strip_comments(text: str) -> str:
    """Blanks out comments, keeping their newlines so line numbers still match."""

    def blank(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith(("//", "/*")):
            return "\n" * token.count("\n")
        return token

    return TOKENS.sub(blank, text)


def call_arguments(text: str, open_paren: int) -> str:
    """The text between the parenthesis at @p open_paren and its match (or to the end, if unbalanced)."""
    depth = 0
    for index in range(open_paren, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1 : index]
    return text[open_paren + 1 :]


def check_text(text: str, name: str) -> list[str]:
    errors: list[str] = []
    code = strip_comments(text)
    for match in LOAD_CALL.finditer(code):
        line = code.count("\n", 0, match.start()) + 1
        call = match.group(0).rstrip("( \t\n")
        if match.group(1) is None:
            errors.append(
                f"{name}:{line}: {call} searches the application directory first; use "
                "LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)"
            )
        elif SEARCH_FLAG not in call_arguments(code, match.end() - 1):
            errors.append(
                f"{name}:{line}: {call} without a LOAD_LIBRARY_SEARCH_* flag searches the application "
                "directory first; pass LOAD_LIBRARY_SEARCH_SYSTEM32"
            )
    return errors


def check_entry_point(text: str, name: str, function: str) -> list[str]:
    """An error unless @p function's definition in @p text starts by restricting the default DLL directories."""
    code = strip_comments(text)
    definition = re.search(r"\b" + re.escape(function) + r"\s*\([^)]*\)[^;{]*\{", code)
    if definition is None:
        return [f"{name}: no definition of {function}() found"]
    line = code.count("\n", 0, definition.start()) + 1
    body = code[definition.end() :]
    position = LEADING_SPACE.match(body).end()
    while (alias := NAMESPACE_ALIAS.match(body, position)) is not None:
        position = LEADING_SPACE.match(body, alias.end()).end()
    call = SET_DEFAULT_DIRS.match(body, position)
    if call is None:
        return [
            f"{name}:{line}: {function}() must first call SetDefaultDllDirectories({SYSTEM32_FLAG}), before "
            "anything can load a DLL by name from TaskSmack's own folder"
        ]
    arguments = call_arguments(body, call.end() - 1)
    if SYSTEM32_FLAG not in arguments or any(flag in arguments for flag in WIDE_FLAGS):
        return [
            f"{name}:{line}: {function}()'s SetDefaultDllDirectories must pass {SYSTEM32_FLAG}, and neither "
            f"{' nor '.join(WIDE_FLAGS)}"
        ]
    return []


def check(root: Path) -> list[str]:
    errors: list[str] = []
    source_dir = root / SOURCE_DIR
    if not source_dir.is_dir():
        return [f"{SOURCE_DIR.as_posix()}: missing"]
    for relative, function in ENTRY_POINTS:
        path = root / relative
        if not path.is_file():
            errors.append(f"{relative.as_posix()}: missing")
            continue
        errors.extend(check_entry_point(path.read_text(encoding="utf-8"), relative.as_posix(), function))
    for path in sorted(p for p in source_dir.rglob("*") if p.suffix in SOURCE_SUFFIXES and p.is_file()):
        text = path.read_text(encoding="utf-8")
        errors.extend(check_text(text, path.relative_to(root).as_posix()))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
