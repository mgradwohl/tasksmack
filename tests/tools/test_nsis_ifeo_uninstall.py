#!/usr/bin/env python3
"""Tests for the uninstaller's Replace Task Manager step (#3072), in cmake/CPackProjectConfig.cmake.in.

The NSIS uninstaller always gives Task Manager back when TaskSmack has it: it removes taskmgr.exe's Image
File Execution Options Debugger when the value is the quoted path of a TaskSmack.exe, never another
program's, and the key only when TaskSmack created it (its TaskSmackCreatedKey mark) and it is empty.

makensis isn't on every machine, and an uninstaller can't be run in a test, so the lines CPack puts in the
uninstaller's section (CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS) are read out of the file and run here by a
small interpreter of the NSIS commands they use, against a fake registry with both of its views. A
command the interpreter doesn't know fails the test, so the step stays within what is checked here.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
IFEO = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe"
INSTALLED = r"C:\Program Files\TaskSmack\TaskSmack.exe"


def uninstall_lines() -> list[str]:
    """The step's lines: CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS's bracket argument."""
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(r"set\(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS \[=\[\n(.*?)\]=\]\)", text, re.DOTALL)
    if match is None:
        raise AssertionError("no CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS in " + str(CONFIG))
    return match.group(1).splitlines()


def tokens(line: str) -> list[str]:
    """An NSIS line's words: quoted with " or ', or bare; a ';' outside quotes starts a comment."""
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


class FakeRegistry:
    """HKLM in its two views: {view: {key: {name: (type, value)}}}, keys compared ignoring case."""

    def __init__(self) -> None:
        self.views: dict[int, dict[str, dict[str, tuple[str, object]]]] = {32: {}, 64: {}}

    def key(self, view: int, name: str) -> dict[str, tuple[str, object]] | None:
        for existing, values in self.views[view].items():
            if existing.lower() == name.lower():
                return values
        return None

    def set(self, view: int, key: str, name: str, kind: str, value: object) -> None:
        values = self.key(view, key)
        if values is None:
            values = self.views[view][key] = {}
        values[name] = (kind, value)

    def delete_key(self, view: int, name: str) -> None:
        for existing in list(self.views[view]):
            if existing.lower() == name.lower():
                del self.views[view][existing]


class Uninstaller:
    """Runs the step's lines as NSIS would, with registers, the error flag and the registry view."""

    def __init__(self, registry: FakeRegistry) -> None:
        self.registry = registry
        self.registers = {"$0": "zero", "$1": "one"}
        self.stack: list[str] = []
        self.error = False
        self.view = 32  # a 32-bit uninstaller's default

    def value(self, word: str) -> str:
        return self.registers.get(word, word)

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
                self.stack.append(self.value(args[0]))
            elif op == "Pop":
                self.registers[args[0]] = self.stack.pop()
            elif op == "SetRegView":
                self.view = {"64": 64, "32": 32, "default": 32}[args[0]]
            elif op == "ClearErrors":
                self.error = False
            elif op in ("ReadRegStr", "ReadRegDWORD"):
                target, root, key, name = args
                assert root == "HKLM", root
                values = self.registry.key(self.view, key) or {}
                kind, data = values.get(name, (None, None))
                wanted = "REG_SZ" if op == "ReadRegStr" else "REG_DWORD"
                if kind is None:
                    self.registers[target], self.error = "", True
                elif kind != wanted:
                    self.registers[target], self.error = str(data), True
                else:
                    self.registers[target] = str(data)
            elif op == "IfErrors":
                nxt = jump(args[0]) if self.error else jump(args[1] if len(args) > 1 else "")
                self.error = False
            elif op == "StrCpy":
                target, source = args[0], self.value(args[1])
                length = args[2] if len(args) > 2 else ""
                offset = int(args[3]) if len(args) > 3 else 0
                text = source[offset:] if offset >= 0 else source[max(len(source) + offset, 0) :]
                self.registers[target] = text if length == "" else text[: int(length)]
            elif op == "StrCmp":
                same = self.value(args[0]).lower() == self.value(args[1]).lower()  # NSIS compares ignoring case
                nxt = jump(args[2]) if same else jump(args[3] if len(args) > 3 else "")
            elif op == "DeleteRegValue":
                root, key, name = args
                assert root == "HKLM" and self.view == 64, "the 64-bit view, where Windows reads IFEO"
                values = self.registry.key(self.view, key)
                if values is not None:
                    values.pop(name, None)
            elif op == "DeleteRegKey":
                assert args[0] == "/ifempty", "never a whole key with what another tool left in it"
                root, key = args[1], args[2]
                assert root == "HKLM" and self.view == 64
                values = self.registry.key(self.view, key)
                if values is not None and not values:
                    self.registry.delete_key(self.view, key)
            else:
                raise AssertionError(f"the test doesn't know NSIS's {op}: teach it, or keep the step to these")
            pc = nxt


class UninstallStepTest(unittest.TestCase):
    def uninstall(self, registry: FakeRegistry) -> Uninstaller:
        uninstaller = Uninstaller(registry)
        uninstaller.run(uninstall_lines())
        self.assertEqual(uninstaller.registers, {"$0": "zero", "$1": "one"}, "its registers are put back")
        self.assertEqual(uninstaller.stack, [])
        self.assertEqual(uninstaller.view, 32, "CPack's own lines after it keep the 32-bit view")
        return uninstaller

    def test_taskmanagers_key_taskSmack_made_goes_altogether(self) -> None:
        registry = FakeRegistry()
        registry.set(64, IFEO, "Debugger", "REG_SZ", f'"{INSTALLED}"')
        registry.set(64, IFEO, "TaskSmackCreatedKey", "REG_DWORD", 1)
        self.uninstall(registry)
        self.assertIsNone(registry.key(64, IFEO))

    def test_any_taskSmacks_debugger_goes_a_portable_ones_too(self) -> None:
        for path in (INSTALLED, r"D:\Tools\TaskSmack\TaskSmack.exe", r"c:\program files\tasksmack\tasksmack.exe"):
            with self.subTest(path=path):
                registry = FakeRegistry()
                registry.set(64, IFEO, "Debugger", "REG_SZ", f'"{path}"')
                self.uninstall(registry)
                self.assertEqual(registry.key(64, IFEO), {}, "the Debugger goes; a key TaskSmack didn't mark stays")

    def test_another_tools_values_and_key_stay(self) -> None:
        registry = FakeRegistry()
        registry.set(64, IFEO, "Debugger", "REG_SZ", f'"{INSTALLED}"')
        registry.set(64, IFEO, "TaskSmackCreatedKey", "REG_DWORD", 1)
        registry.set(64, IFEO, "GlobalFlag", "REG_DWORD", 0x200)
        self.uninstall(registry)
        self.assertEqual(registry.key(64, IFEO), {"GlobalFlag": ("REG_DWORD", 0x200)})

    def test_another_programs_debugger_is_left_alone(self) -> None:
        for debugger in (
            r'"C:\Tools\procexp64.exe"',
            r"C:\Tools\procexp64.exe /e",
            r'"C:\Tools\NotTaskSmack.exe"',
            r'"C:\Tools\TaskSmack.exe" /x',
            r'"C:\Tools\TaskSmack.exe.bak"',
            "TaskSmack.exe",
            "",
        ):
            with self.subTest(debugger=debugger):
                registry = FakeRegistry()
                registry.set(64, IFEO, "Debugger", "REG_SZ", debugger)
                registry.set(64, IFEO, "TaskSmackCreatedKey", "REG_DWORD", 1)
                self.uninstall(registry)
                self.assertEqual(registry.key(64, IFEO)["Debugger"], ("REG_SZ", debugger))
                self.assertIn("TaskSmackCreatedKey", registry.key(64, IFEO))

    def test_an_unquoted_path_isnt_one_TaskSmack_wrote(self) -> None:
        registry = FakeRegistry()
        registry.set(64, IFEO, "Debugger", "REG_SZ", INSTALLED)
        self.uninstall(registry)
        self.assertIn("Debugger", registry.key(64, IFEO))

    def test_a_mark_that_isnt_TaskSmacks_keeps_the_key(self) -> None:
        for mark in (("REG_SZ", "1"), ("REG_DWORD", 0), ("REG_DWORD", 2)):
            with self.subTest(mark=mark):
                registry = FakeRegistry()
                registry.set(64, IFEO, "Debugger", "REG_SZ", f'"{INSTALLED}"')
                registry.set(64, IFEO, "TaskSmackCreatedKey", *mark)
                self.uninstall(registry)
                self.assertEqual(registry.key(64, IFEO), {"TaskSmackCreatedKey": mark})

    def test_nothing_there_is_no_error(self) -> None:
        registry = FakeRegistry()
        self.uninstall(registry)
        self.assertEqual(registry.views, {32: {}, 64: {}})

    def test_only_the_64_bit_view_is_touched(self) -> None:
        registry = FakeRegistry()
        registry.set(32, IFEO, "Debugger", "REG_SZ", f'"{INSTALLED}"')
        registry.set(32, IFEO, "TaskSmackCreatedKey", "REG_DWORD", 1)
        self.uninstall(registry)
        self.assertEqual(len(registry.key(32, IFEO)), 2, "Wow6432Node isn't where Windows reads it")

    def test_it_is_the_nsis_generators_only(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        block = text.index('if(CPACK_GENERATOR STREQUAL "NSIS")')
        step = text.index("set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS")
        self.assertLess(block, step)
        self.assertNotIn("endif()", text[block:step])


if __name__ == "__main__":
    unittest.main()
