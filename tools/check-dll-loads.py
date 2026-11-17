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
  - Each Windows entry point (ENTRY_POINTS, the images that can run elevated, #3897) must call
    SetDefaultDllDirectories as its first statement (namespace aliases aside), never with
    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS or LOAD_LIBRARY_SEARCH_USER_DIRS, and by its own rule:
      - TaskSmackHelper.exe's wWinMain (EXACT): exactly LOAD_LIBRARY_SEARCH_SYSTEM32, never its own
        folder, since it always runs elevated;
      - TaskSmack.exe's WinMain (BRANCH): a conditional with a System32-only branch,
        "? LOAD_LIBRARY_SEARCH_SYSTEM32 :", the one it takes elevated (DllSearchPolicy.h), and then,
        as its very next statement, SdlEnvironment::removeForThisProcess( (#4103), passed
        system32OnlyForThisProcess() and not another value, with SetDefaultDllDirectories a statement of
        its own (nothing but whitespace between its ")" and ";", #4114): an elevated TaskSmack drops the SDL variables that name a DLL SDL loads by full path (SDL3_DYNAMIC_API,
        read at SDL's first call, and SDL_OPENGL_LIBRARY) before anything can make an SDL call. Its
        SetDefaultDllDirectories argument must be exactly "DllSearchPolicy::system32OnlyForThisProcess() ?
        LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32" (so no
        SDL_ identifier or other call runs before the scrub), the scrub is its only removeForThisProcess( call, and
        the statement after it is the fail-closed "if (... !x.empty())" over stillSetInThisProcess() that returns
        (#4169). A [[nodiscard]] result would be enforced by the compiler but not that WinMain returns on it, and
        would reshape the API and its tests; this rule checks the return, which is the part that matters.
    That restricts what third-party code (SDL, the GL loaders) and the DLLs it loads find by name;
    the linker's /DEPENDENTLOADFLAG:0x800 covers the imports (cmake/CompilerOptions.cmake).

Comments are ignored. This is a text scan of the C++ sources under src/, not a parse, so it needs
nothing beyond the standard library. For the same reason it can't see a macro or a using-declaration
that redefines system32OnlyForThisProcess or SetDefaultDllDirectories; review catches that.

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

# How an entry point's SetDefaultDllDirectories argument must read: exactly System32, or a conditional with a
# System32-only branch.
EXACT = "exact"
BRANCH = "branch"
# The entry points that must restrict the default DLL directories first (#3897): file, function name, rule.
ENTRY_POINTS = (
    (Path("src/main.cpp"), "WinMain", BRANCH),
    (Path("src/Platform/Windows/HelperMain.cpp"), "wWinMain", EXACT),
)
SYSTEM32_FLAG = "LOAD_LIBRARY_SEARCH_SYSTEM32"
SYSTEM32_BRANCH = re.compile(r"\?\s*LOAD_LIBRARY_SEARCH_SYSTEM32\s*:")
WIDE_FLAGS = ("LOAD_LIBRARY_SEARCH_DEFAULT_DIRS", "LOAD_LIBRARY_SEARCH_USER_DIRS")
SET_DEFAULT_DIRS = re.compile(r"SetDefaultDllDirectories\s*\(")
# What TaskSmack.exe's WinMain (BRANCH) must call right after SetDefaultDllDirectories, qualified or not (#4103).
SDL_SCRUB = "SdlEnvironment::removeForThisProcess("
SDL_SCRUB_CALL = re.compile(r"(?:\w+\s*::\s*)*SdlEnvironment\s*::\s*removeForThisProcess\s*\(")
# What that call must be passed: the rule WinMain's SetDefaultDllDirectories uses (#4114).
SCRUB_ARGUMENT = "system32OnlyForThisProcess()"
# The rule's call, as written: the qualifier is pinned to DllSearchPolicy::, optionally under Platform::Windows:: and
# a leading global "::" (#4169), so another namespace's system32OnlyForThisProcess() or a bare name is rejected.
POLICY_CALL = (
    r"(?:::\s*)?(?:Platform\s*::\s*Windows\s*::\s*)?DllSearchPolicy\s*::\s*system32OnlyForThisProcess\s*\(\s*\)"
)
# The whole argument of the scrub: nothing negating it, combining it, or merely containing its name (#4114).
SCRUB_ARGUMENT_SHAPE = re.compile(POLICY_CALL)
# The one shape WinMain's SetDefaultDllDirectories argument may have (#4169): the rule picks System32 only, or the
# application folder as well.
SET_DEFAULT_ARGUMENT_SHAPE = re.compile(
    POLICY_CALL + r"\s*\?\s*LOAD_LIBRARY_SEARCH_SYSTEM32\s*:\s*"
    r"LOAD_LIBRARY_SEARCH_APPLICATION_DIR\s*\|\s*LOAD_LIBRARY_SEARCH_SYSTEM32"
)
SDL_IDENTIFIER = re.compile(r"\bSDL\d*_\w*")
CALLEE = re.compile(r"(\w+)\s*(?:<[^()]*>)?\s*\(")
# WinMain's check right after the scrub: "if (... !x.empty())" over stillSetInThisProcess(), whose block returns (#4169).
STILL_SET_CALL = re.compile(r"\bstillSetInThisProcess\s*\(\s*\)")
NOT_EMPTY = re.compile(r"!\s*\w+\s*\.\s*empty\s*\(\s*\)")
RETURN = re.compile(r"\breturn\b")
IF_STATEMENT = re.compile(r"if\s*\(")
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


def blank_literals(text: str) -> str:
    """@p text with each string and character literal blanked, same length, so a word or brace inside one is neither
    code nor a match."""
    return TOKENS.sub(lambda match: " " * len(match.group(0)), text)


def matching_brace(text: str, open_brace: int) -> int:
    """The index of the brace closing the one at @p open_brace (or the end of @p text, if unbalanced). String and
    character literals are skipped, so a brace in one doesn't count."""
    blanked = blank_literals(text)
    depth = 0
    for index in range(open_brace, len(blanked)):
        if blanked[index] == "{":
            depth += 1
        elif blanked[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


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


def check_fail_closed(body: str, position: int) -> str | None:
    """Why the text at @p position in @p body (the function's body, after the scrub) isn't the fail-closed check, or None
    if it is: "if (... !x.empty())" over SdlEnvironment::stillSetInThisProcess() whose block returns (#4169). Without it
    a scrub that couldn't clear a variable lets TaskSmack run on to SDL."""
    guard = IF_STATEMENT.match(body, position)
    if guard is None:
        return (
            "the statement after the SDL scrub must be the fail-closed check, 'if (... "
            "stillSetInThisProcess() ... !x.empty())' returning a failure (#4169)"
        )
    # Searched with literals blanked: a word inside a string ("will not return") is not code (#4169)
    code = blank_literals(body)
    condition = call_arguments(code, guard.end() - 1)
    if STILL_SET_CALL.search(condition) is None or NOT_EMPTY.search(condition) is None:
        return "the check after the SDL scrub must test !<names>.empty() of SdlEnvironment::stillSetInThisProcess() (#4169)"
    open_brace = LEADING_SPACE.match(code, guard.end() + len(condition) + 1).end()
    if not code.startswith("{", open_brace):
        return "the check after the SDL scrub must have a braced block that returns (#4169)"
    if RETURN.search(code[open_brace : matching_brace(code, open_brace) + 1]) is None:
        return "the check after the SDL scrub must return a failure when a name is still set (#4169)"
    return None


def check_entry_point(text: str, name: str, function: str, rule: str) -> list[str]:
    """An error unless @p function's definition in @p text starts by restricting the default DLL directories, as
    @p rule (EXACT or BRANCH) says."""
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
    if any(flag in arguments for flag in WIDE_FLAGS):
        return [f"{name}:{line}: {function}()'s SetDefaultDllDirectories must pass neither {' nor '.join(WIDE_FLAGS)}"]
    if rule == EXACT and arguments.strip() != SYSTEM32_FLAG:
        return [f"{name}:{line}: {function}()'s SetDefaultDllDirectories must pass exactly {SYSTEM32_FLAG}"]
    if rule == BRANCH and SYSTEM32_BRANCH.search(arguments) is None:
        return [
            f"{name}:{line}: {function}()'s SetDefaultDllDirectories must keep a System32-only branch, "
            f"'? {SYSTEM32_FLAG} :', for an elevated TaskSmack"
        ]
    if rule == BRANCH:
        stray = [m.group(0) for m in SDL_IDENTIFIER.finditer(arguments)]
        stray += [m.group(1) + "(" for m in CALLEE.finditer(arguments) if m.group(1) != SCRUB_ARGUMENT.rstrip("()")]
        if stray:
            return [
                f"{name}:{line}: {function}()'s SetDefaultDllDirectories arguments must hold no SDL_ identifier and "
                f"no call but {SCRUB_ARGUMENT} (found {', '.join(stray)}): nothing may run before the scrub (#4169)"
            ]
        if SET_DEFAULT_ARGUMENT_SHAPE.fullmatch(arguments.strip()) is None:
            return [
                f"{name}:{line}: {function}()'s SetDefaultDllDirectories must be passed exactly "
                f"'DllSearchPolicy::{SCRUB_ARGUMENT} ? {SYSTEM32_FLAG} : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | "
                f"{SYSTEM32_FLAG}', optionally under Platform::Windows:: or a leading '::' (#4169)"
            ]
    if rule == BRANCH:
        close = call.end() - 1 + len(arguments) + 1
        statement_end = body.find(";", close)
        # Nothing but whitespace between the call's ")" and its ";": no "SetDefaultDllDirectories(...), SDL_SetHint(...);" (#4114)
        if statement_end < 0 or body[close + 1 : statement_end].strip() != "":
            return [
                f"{name}:{line}: {function}()'s SetDefaultDllDirectories call must be a statement of its own: only "
                "whitespace between its closing parenthesis and its ';' (#4114)"
            ]
        following = LEADING_SPACE.match(body, statement_end + 1).end()
        scrub = SDL_SCRUB_CALL.match(body, following)
        if scrub is None:
            return [
                f"{name}:{line}: {function}()'s next statement after SetDefaultDllDirectories must be "
                f"{SDL_SCRUB}...): an elevated TaskSmack removes the SDL variables (SDL3_DYNAMIC_API, read at SDL's "
                "first call, SDL_OPENGL_LIBRARY) that name a DLL SDL loads by full path, before any SDL call (#4103)"
            ]
        scrub_arguments = call_arguments(body, scrub.end() - 1)
        if SCRUB_ARGUMENT_SHAPE.fullmatch(scrub_arguments.strip()) is None:
            return [
                f"{name}:{line}: {function}()'s {SDL_SCRUB}...) must be passed {SCRUB_ARGUMENT}, the rule "
                "SetDefaultDllDirectories uses, not another value (#4114)"
            ]
        scrub_close = scrub.end() - 1 + len(scrub_arguments) + 1
        scrub_end = body.find(";", scrub_close)
        if scrub_end < 0 or body[scrub_close + 1 : scrub_end].strip() != "":
            return [
                f"{name}:{line}: {function}()'s {SDL_SCRUB}...) call must be a statement of its own: only whitespace "
                "between its closing parenthesis and its ';' (#4169)"
            ]
        failure = check_fail_closed(body, LEADING_SPACE.match(body, scrub_end + 1).end())
        if failure is not None:
            return [f"{name}:{line}: {function}(): {failure}"]
        function_open = definition.end() - 1
        function_body = code[function_open : matching_brace(code, function_open) + 1]
        if len(SDL_SCRUB_CALL.findall(function_body)) != 1:
            return [
                f"{name}:{line}: {function}() must call {SDL_SCRUB}...) exactly once: a second call before the check "
                "clears the names still set (#4169)"
            ]
    return []


def check(root: Path) -> list[str]:
    errors: list[str] = []
    source_dir = root / SOURCE_DIR
    if not source_dir.is_dir():
        return [f"{SOURCE_DIR.as_posix()}: missing"]
    for relative, function, rule in ENTRY_POINTS:
        path = root / relative
        if not path.is_file():
            errors.append(f"{relative.as_posix()}: missing")
            continue
        errors.extend(check_entry_point(path.read_text(encoding="utf-8"), relative.as_posix(), function, rule))
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
