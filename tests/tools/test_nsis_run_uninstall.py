#!/usr/bin/env python3
"""Tests for the uninstaller's start-at-sign-in step (#3093, part 2), in cmake/CPackProjectConfig.cmake.in.

The NSIS uninstaller deletes the HKCU Run key's TaskSmack value when it starts this installation's
TaskSmack.exe ("$INSTDIR\\TaskSmack.exe", quoted), and leaves a value for another copy or another program,
and every other value, alone.

makensis isn't on every machine, and an uninstaller can't be run in a test, so the step's lines (the block
between its BEGIN and END markers, which CPack appends to CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS) are read out
of the file and run here by a small interpreter of the NSIS commands they use, against a fake registry. A
command the interpreter doesn't know fails the test, so the step stays within what is checked here.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
RUN = r"Software\Microsoft\Windows\CurrentVersion\Run"
INSTDIR = r"C:\Program Files\TaskSmack"
BEGIN = "; TaskSmack's start at sign-in (#3093): BEGIN"
END = "; TaskSmack's start at sign-in (#3093): END"


def step_lines() -> list[str]:
    """The step's lines, from its BEGIN marker to its END marker."""
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(
        r"string\(APPEND CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS \[=\[\n(.*?)\]=\]\)", text, re.DOTALL
    )
    if match is None:
        raise AssertionError("no string(APPEND CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS ...) in " + str(CONFIG))
    lines = match.group(1).splitlines()
    stripped = [line.strip() for line in lines]
    if BEGIN not in stripped or END not in stripped:
        raise AssertionError("the step's BEGIN and END markers are missing")
    return lines[stripped.index(BEGIN) : stripped.index(END) + 1]


def tokens(line: str) -> list[str]:
    """An NSIS line's words: quoted with ", ' or `, or bare; a ';' outside quotes starts a comment."""
    words, i = [], 0
    while i < len(line):
        c = line[i]
        if c.isspace():
            i += 1
        elif c == ";":
            break
        elif c in "\"'`":
            end = line.index(c, i + 1)
            words.append(line[i + 1 : end])
            i = end + 1
        else:
            end = i
            while end < len(line) and not line[end].isspace():
                end += 1
            words.append(line[i:end])
            i = end
    return words


class Uninstaller:
    """Runs the step's lines as NSIS would: registers, a stack, the error flag and HKCU's values."""

    def __init__(self, hkcu: dict[str, dict[str, str]], instdir: str = INSTDIR) -> None:
        self.hkcu = hkcu
        self.vars = {"$INSTDIR": instdir, "$0": "zero", "$1": "one", "$2": "two", "$3": "three"}
        self.stack: list[str] = []
        self.error = False
        self.deleted: list[tuple[str, str]] = []

    def expand(self, word: str) -> str:
        # Longest names first, so $INSTDIR isn't read as $I...
        for name in sorted(self.vars, key=len, reverse=True):
            word = word.replace(name, self.vars[name])
        return word

    def run(self, lines: list[str]) -> None:
        program = [tokens(line) for line in lines]
        labels = {words[0][:-1]: i for i, words in enumerate(program) if len(words) == 1 and words[0].endswith(":")}
        pc = 0
        steps = 0

        def jump(target: str) -> int:
            if target in ("", "0"):
                return pc + 1
            if target not in labels:
                raise AssertionError(f"no label {target}")
            return labels[target]

        while pc < len(program):
            steps += 1
            if steps > 1000:
                raise AssertionError("the step doesn't end")
            words = program[pc]
            if not words or (len(words) == 1 and words[0].endswith(":")):
                pc += 1
                continue
            op, args = words[0], words[1:]
            nxt = pc + 1
            if op == "Push":
                self.stack.append(self.expand(args[0]))
            elif op == "Pop":
                self.vars[args[0]] = self.stack.pop()
            elif op == "ClearErrors":
                self.error = False
            elif op == "ReadRegStr":
                var, root, key, name = args
                self.assert_hkcu(root, key)
                value = self.hkcu.get(key.lower(), {}).get(name.lower())
                if value is None:
                    self.vars[var] = ""
                    self.error = True
                else:
                    self.vars[var] = value
            elif op == "IfErrors":
                nxt = jump(args[0]) if self.error else jump(args[1] if len(args) > 1 else "")
            elif op == "StrCpy":
                value = self.expand(args[1])
                if len(args) > 2:
                    length = int(self.expand(args[2]) or "0")
                    start = int(self.expand(args[3])) if len(args) > 3 else 0
                    value = value[start:]
                    if length > 0:
                        value = value[:length]
                    elif length < 0:
                        value = value[: len(value) + length]
                self.vars[args[0]] = value
            elif op == "StrLen":
                self.vars[args[0]] = str(len(self.expand(args[1])))
            elif op == "StrCmp":
                # NSIS compares ignoring case.
                same = self.expand(args[0]).lower() == self.expand(args[1]).lower()
                nxt = jump(args[2]) if same else jump(args[3] if len(args) > 3 else "")
            elif op == "DeleteRegValue":
                root, key, name = args
                self.assert_hkcu(root, key)
                values = self.hkcu.get(key.lower(), {})
                if name.lower() in values:
                    del values[name.lower()]
                self.deleted.append((key, name))
            else:
                raise AssertionError(f"the interpreter doesn't know {op}")
            pc = nxt

    @staticmethod
    def assert_hkcu(root: str, key: str) -> None:
        if root != "HKCU" or key.lower() != RUN.lower():
            raise AssertionError(f"the step touches {root}\\{key}, not the per-user Run key")


def run_step(values: dict[str, str], instdir: str = INSTDIR) -> Uninstaller:
    hkcu = {RUN.lower(): {name.lower(): value for name, value in values.items()}}
    uninstaller = Uninstaller(hkcu, instdir)
    uninstaller.run(step_lines())
    return uninstaller


class RunValueUninstallTest(unittest.TestCase):
    def test_this_installations_value_is_deleted(self) -> None:
        u = run_step({"TaskSmack": rf'"{INSTDIR}\TaskSmack.exe" --minimized'})
        self.assertNotIn("tasksmack", u.hkcu[RUN.lower()])

    def test_it_matches_ignoring_case_and_without_arguments(self) -> None:
        u = run_step({"TaskSmack": rf'"{INSTDIR.upper()}\taskSMACK.EXE"'})
        self.assertNotIn("tasksmack", u.hkcu[RUN.lower()])

    def test_another_copys_value_is_left(self) -> None:
        for other in (
            r'"D:\Tools\TaskSmack\TaskSmack.exe" --minimized',
            rf'"{INSTDIR}\TaskSmack.exe.old" --minimized',
            rf"{INSTDIR}\TaskSmack.exe --minimized",
            rf'"{INSTDIR}\Other.exe"',
            "",
        ):
            with self.subTest(other=other):
                u = run_step({"TaskSmack": other})
                self.assertEqual(u.hkcu[RUN.lower()].get("tasksmack"), other)
                self.assertEqual(u.deleted, [])

    def test_no_value_is_nothing_to_do(self) -> None:
        u = run_step({})
        self.assertEqual(u.deleted, [])

    def test_other_values_are_never_touched(self) -> None:
        u = run_step({"TaskSmack": rf'"{INSTDIR}\TaskSmack.exe" --minimized', "OneDrive": "onedrive.exe /background"})
        self.assertEqual(u.hkcu[RUN.lower()], {"onedrive": "onedrive.exe /background"})
        self.assertEqual(u.deleted, [(RUN, "TaskSmack")])

    def test_the_registers_are_given_back(self) -> None:
        for values in ({}, {"TaskSmack": "x"}, {"TaskSmack": rf'"{INSTDIR}\TaskSmack.exe"'}):
            with self.subTest(values=values):
                u = run_step(values)
                self.assertEqual(
                    {name: u.vars[name] for name in ("$0", "$1", "$2", "$3")},
                    {"$0": "zero", "$1": "one", "$2": "two", "$3": "three"},
                )
                self.assertEqual(u.stack, [])

    def test_the_value_matches_what_tasksmack_writes(self) -> None:
        # Domain::StartAtLogin::runCommand(): the quoted path, a space, then --minimized.
        written = '"' + INSTDIR + r'\TaskSmack.exe" --minimized'
        u = run_step({"TaskSmack": written})
        self.assertEqual(u.deleted, [(RUN, "TaskSmack")])


class TokensTest(unittest.TestCase):
    def test_quotes_and_comments(self) -> None:
        self.assertEqual(tokens("StrCmp $1 '\"x\"' 0 done ; note"), ["StrCmp", "$1", '"x"', "0", "done"])


if __name__ == "__main__":
    unittest.main()
