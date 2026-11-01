#!/usr/bin/env python3
"""Tests for the installer's upgrade step (#3276), in cmake/CPackProjectConfig.cmake.in.

Before it copies the new files, the NSIS installer uninstalls the version that is there itself (CPack's own
uninstall-before-install is off, cmake/Packaging.cmake). When the new version goes to the same folder it
runs the old uninstaller with /TASKSMACK_UPGRADE, and the uninstaller's lines for Replace Task Manager
(#3072) and Start at sign-in (#3093) keep taskmgr.exe's Debugger and the Run value: they name that folder's
TaskSmack.exe, which the install puts back. Installed elsewhere, it passes /TASKSMACK_MOVE: both values go,
since they name a TaskSmack.exe that is gone. Only a real uninstall, with neither flag, leaves the uninstall
mark (the HKCU value Uninstalled under Software, TaskSmack), which makes the next TaskSmack forget what the user had turned on
rather than write it back after a later fresh install.

makensis isn't on every machine, and an installer can't be run in a test, so the step's lines (the block
CPack puts in the installer's section, CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS) are read out of the file and
run here by a small interpreter of the NSIS commands they use. The uninstaller it starts is then run as
NSIS would run it, with the command line the step passes ($CMDLINE ends before _?=, which sets $INSTDIR),
through the uninstall lines' own interpreters (test_nsis_ifeo_uninstall.py, test_nsis_run_uninstall.py).
"""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_nsis_ifeo_uninstall as ifeo  # noqa: E402
import test_nsis_run_uninstall as run  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
PACKAGING = REPO / "cmake" / "Packaging.cmake"
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskSmack"
INSTDIR = r"C:\Program Files\TaskSmack"
UNINSTALLER = INSTDIR + r"\Uninstall.exe"
REGISTERS = {"$0": "zero", "$1": "one"}
MARK_KEY = r"Software\TaskSmack"
MARK_BEGIN = "; TaskSmack's uninstall mark (#3276): BEGIN"
MARK_END = "; TaskSmack's uninstall mark (#3276): END"


def step_lines() -> list[str]:
    """The step's lines: CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS's bracket argument."""
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(r"set\(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS \[=\[\n(.*?)\]=\]\)", text, re.DOTALL)
    if match is None:
        raise AssertionError("no CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS in " + str(CONFIG))
    return match.group(1).splitlines()


class Installer:
    """Runs the step's lines as NSIS would: registers, a stack, the error flag, HKLM's Uninstall key (in the
    32-bit installer's default view) and the files that exist. ExecWait records the command it starts."""

    def __init__(self, uninstall_string: str | None, instdir: str, files: set[str], can_start: bool = True) -> None:
        self.uninstall_string = uninstall_string
        self.files = {name.lower() for name in files}
        self.can_start = can_start
        self.vars = {"$INSTDIR": instdir, **REGISTERS}
        self.stack: list[str] = []
        self.error = False
        self.executed: list[str] = []
        self.out_paths: list[str] = []
        self.messages: list[str] = []
        self.aborted = False

    def expand(self, word: str) -> str:
        for name in sorted(self.vars, key=len, reverse=True):
            word = word.replace(name, self.vars[name])
        return word

    def run(self, lines: list[str]) -> None:
        program = [ifeo.tokens(line) for line in lines]
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
                if root != "HKLM" or key != UNINSTALL_KEY or name != "UninstallString":
                    raise AssertionError(f"the step reads {root}\\{key} {name}, not the Uninstall key's UninstallString")
                if self.uninstall_string is None:
                    self.vars[var], self.error = "", True
                else:
                    self.vars[var] = self.uninstall_string
            elif op == "IfErrors":
                nxt = jump(args[0]) if self.error else jump(args[1] if len(args) > 1 else "")
            elif op == "StrCpy":
                value = self.expand(args[1])
                if len(args) > 2:
                    length_text = self.expand(args[2])
                    start = int(self.expand(args[3])) if len(args) > 3 else 0
                    value = value[start:] if start >= 0 else value[max(len(value) + start, 0) :]
                    if length_text != "":
                        length = int(length_text)
                        value = value[:length] if length >= 0 else value[: max(len(value) + length, 0)]
                self.vars[args[0]] = value
            elif op == "StrCmp":
                same = self.expand(args[0]).lower() == self.expand(args[1]).lower()  # NSIS compares ignoring case
                nxt = jump(args[2]) if same else jump(args[3] if len(args) > 3 else "")
            elif op == "IfFileExists":
                exists = self.expand(args[0]).lower() in self.files
                nxt = jump(args[1]) if exists else jump(args[2] if len(args) > 2 else "")
            elif op == "Goto":
                nxt = jump(args[0])
            elif op == "DetailPrint":
                pass
            elif op == "ExecWait":
                self.executed.append(self.expand(args[0]))
                if not self.can_start:
                    self.error = True
            elif op == "MessageBox":
                if "/SD" not in args:
                    raise AssertionError("a message box needs /SD, for a silent install")
                self.messages.append(self.expand(args[1]))
            elif op == "Abort":
                self.aborted = True
                return
            elif op == "SetOutPath":
                self.out_paths.append(self.expand(args[0]))
            else:
                raise AssertionError(f"the interpreter doesn't know {op}")
            pc = nxt


def mark_lines() -> list[str]:
    """The uninstall mark's lines, from its BEGIN marker to its END marker."""
    lines = CONFIG.read_text(encoding="utf-8").splitlines()
    stripped = [line.strip() for line in lines]
    if MARK_BEGIN not in stripped or MARK_END not in stripped:
        raise AssertionError("the uninstall mark's BEGIN and END markers are missing")
    return lines[stripped.index(MARK_BEGIN) : stripped.index(MARK_END) + 1]


class MarkStep:
    """Runs the uninstall mark's lines: $CMDLINE, two registers, and the HKCU values it may write."""

    def __init__(self, cmdline: str) -> None:
        self.vars = {"$CMDLINE": cmdline, "$1": "one", "$2": "two"}
        self.stack: list[str] = []
        self.hkcu: dict[tuple[str, str], int] = {}

    def value(self, word: str) -> str:
        return self.vars.get(word, word)

    def run(self, lines: list[str]) -> None:
        program = [ifeo.tokens(line) for line in lines]
        labels = {words[0][:-1]: i for i, words in enumerate(program) if len(words) == 1 and words[0].endswith(":")}
        pc = 0
        steps = 0
        while pc < len(program):
            steps += 1
            if steps > 10000:
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
                self.vars[args[0]] = self.stack.pop()
            elif op == "StrCpy":
                if len(args) == 2:
                    self.vars[args[0]] = self.value(args[1])
                else:
                    text = self.value(args[1])[int(self.value(args[3])) :]
                    self.vars[args[0]] = text[: int(self.value(args[2]))]
            elif op == "StrCmp":
                if self.value(args[0]).lower() == self.value(args[1]).lower():
                    nxt = labels[args[2]] if args[2] not in ("", "0") else pc + 1
                elif len(args) > 3 and args[3] not in ("", "0"):
                    nxt = labels[args[3]]
            elif op == "IntOp":
                assert args[2] == "+", args
                self.vars[args[0]] = str(int(self.value(args[1])) + int(self.value(args[3])))
            elif op == "Goto":
                nxt = labels[args[0]]
            elif op == "WriteRegDWORD":
                root, key, name, data = args
                if root != "HKCU" or key != MARK_KEY or name != "Uninstalled":
                    raise AssertionError(f"the mark goes to HKCU\\{MARK_KEY} Uninstalled, not {root}\\{key} {name}")
                self.hkcu[(key, name)] = int(data)
            else:
                raise AssertionError(f"the interpreter doesn't know {op}")
            pc = nxt


def uninstaller_call(command: str) -> tuple[str, str]:
    """The uninstaller's $CMDLINE and $INSTDIR for the command ExecWait started: NSIS cuts " _?=<dir>", the
    last argument, off the command line and installs nothing from %TEMP%."""
    match = re.fullmatch(r"(.*) _\?=(.*)", command)
    if match is None:
        raise AssertionError(f"the uninstaller isn't run in place (_?=): {command}")
    return match.group(1), match.group(2)


class Machine:
    """What the two uninstall steps look after: taskmgr.exe's Debugger (HKLM, 64-bit view) and the Run value."""

    def __init__(self, folder: str) -> None:
        exe = folder + r"\TaskSmack.exe"
        self.ifeo = ifeo.FakeRegistry()
        self.ifeo.set(64, ifeo.IFEO, "Debugger", "REG_SZ", f'"{exe}"')
        self.ifeo.set(64, ifeo.IFEO, "TaskSmackCreatedKey", "REG_DWORD", 1)
        self.run_value = f'"{exe}" --minimized'
        self.hkcu = {run.RUN.lower(): {"tasksmack": self.run_value}}
        self.mark: dict[tuple[str, str], int] = {}

    def uninstall(self, command: str) -> None:
        cmdline, instdir = uninstaller_call(command)
        ifeo.Uninstaller(self.ifeo, cmdline).run(ifeo.uninstall_lines())
        run.Uninstaller(self.hkcu, instdir, cmdline).run(run.step_lines())
        step = MarkStep(cmdline)
        step.run(mark_lines())
        assert step.vars["$1"] == "one" and step.vars["$2"] == "two" and step.stack == [], "its registers are put back"
        self.mark.update(step.hkcu)

    def debugger(self) -> object:
        return (self.ifeo.key(64, ifeo.IFEO) or {}).get("Debugger")

    def run_entry(self) -> str | None:
        return self.hkcu[run.RUN.lower()].get("tasksmack")


def install(uninstall_string: str | None, instdir: str = INSTDIR, files: set[str] | None = None, can_start: bool = True) -> Installer:
    installer = Installer(uninstall_string, instdir, {UNINSTALLER} if files is None else files, can_start)
    installer.run(step_lines())
    return installer


class UpgradeStepTest(unittest.TestCase):
    def assert_registers_back(self, installer: Installer) -> None:
        self.assertEqual({name: installer.vars[name] for name in REGISTERS}, REGISTERS, "its registers are put back")
        self.assertEqual(installer.stack, [])

    def test_an_upgrade_in_place_keeps_both_values(self) -> None:
        installer = install(f'"{UNINSTALLER}"')
        self.assertEqual(installer.executed, [f'"{UNINSTALLER}" /S /TASKSMACK_UPGRADE _?={INSTDIR}'])
        self.assertEqual(installer.out_paths, [INSTDIR], "the install's folder is made again for its files")
        self.assertFalse(installer.aborted)
        self.assert_registers_back(installer)

        machine = Machine(INSTDIR)
        machine.uninstall(installer.executed[0])
        self.assertEqual(machine.debugger(), ("REG_SZ", f'"{INSTDIR}\\TaskSmack.exe"'))
        self.assertEqual(machine.run_entry(), machine.run_value)
        self.assertEqual(machine.mark, {}, "an upgrade is no uninstall")

    def test_the_folder_matches_ignoring_case_and_an_unquoted_uninstall_string(self) -> None:
        # CPack writes it quoted; older CMake didn't, and CPack's own .onInit took both.
        lower = r"c:\program files\tasksmack\uninstall.exe"
        installer = install(lower, files={lower})
        self.assertEqual(installer.executed, [f'"{lower}" /S /TASKSMACK_UPGRADE _?=c:\\program files\\tasksmack'])

    def test_installing_elsewhere_is_an_uninstall(self) -> None:
        elsewhere = r"D:\Apps\TaskSmack"
        installer = install(f'"{UNINSTALLER}"', instdir=elsewhere)
        self.assertEqual(installer.executed, [f'"{UNINSTALLER}" /S /TASKSMACK_MOVE _?={INSTDIR}'])
        self.assertEqual(installer.out_paths, [elsewhere])
        self.assert_registers_back(installer)

        # The old folder's TaskSmack.exe is gone: neither value may start it. But it is no uninstall: no mark,
        # so the new copy may write the Run value back for itself.
        machine = Machine(INSTDIR)
        machine.uninstall(installer.executed[0])
        self.assertIsNone(machine.debugger())
        self.assertIsNone(machine.run_entry())
        self.assertEqual(machine.mark, {})

    def test_a_users_uninstall_still_removes_both(self) -> None:
        # What an uninstall from Settings > Apps runs, after NSIS copied it to %TEMP%.
        machine = Machine(INSTDIR)
        machine.uninstall(rf'"C:\Users\me\AppData\Local\Temp\~nsuA.tmp\Un_A.exe" /S _?={INSTDIR}')
        self.assertIsNone(machine.debugger())
        self.assertIsNone(machine.run_entry())
        # #3276: and leaves the mark, so a later fresh install doesn't turn either back on.
        self.assertEqual(machine.mark, {(MARK_KEY, "Uninstalled"): 1})

    def test_only_a_real_uninstall_leaves_the_mark(self) -> None:
        for cmdline, marked in (
            (r'"C:\Temp\Un_A.exe"', True),
            (r'"C:\Temp\Un_A.exe" /S', True),
            (r'"C:\Temp\Un_A.exe" /S /TASKSMACK_UPGRADE', False),
            (r'"C:\Temp\Un_A.exe" /S /tasksmack_move', False),
            (r'"C:\Temp\Un_A.exe" /S /TASKSMACK_MOV', True),
            ("", True),
        ):
            with self.subTest(cmdline=cmdline):
                step = MarkStep(cmdline)
                step.run(mark_lines())
                self.assertEqual(step.hkcu, {(MARK_KEY, "Uninstalled"): 1} if marked else {})
                self.assertEqual((step.vars["$1"], step.vars["$2"], step.stack), ("one", "two", []))

    def test_a_first_install_runs_nothing(self) -> None:
        installer = install(None)
        self.assertEqual(installer.executed, [])
        self.assertEqual(installer.out_paths, [])
        self.assertFalse(installer.aborted)
        self.assert_registers_back(installer)

    def test_an_uninstall_string_that_isnt_an_uninstaller_there_is_left_alone(self) -> None:
        for uninstall_string, files in (
            ("", {UNINSTALLER}),
            (f'"{UNINSTALLER}"', set()),
            (r'"C:\Program Files\TaskSmack\Other.exe"', {r"C:\Program Files\TaskSmack\Other.exe"}),
            ('"', set()),
        ):
            with self.subTest(uninstall_string=uninstall_string, files=files):
                installer = install(uninstall_string, files=files)
                self.assertEqual(installer.executed, [])
                self.assertFalse(installer.aborted, "the install goes on over it")
                self.assert_registers_back(installer)

    def test_an_uninstaller_that_cant_start_stops_the_install(self) -> None:
        installer = install(f'"{UNINSTALLER}"', can_start=False)
        self.assertEqual(len(installer.executed), 1)
        self.assertEqual(installer.messages, ["Uninstall failed."])
        self.assertTrue(installer.aborted)

    def test_cpacks_own_uninstall_before_install_is_off(self) -> None:
        # CPack's .onInit call can't pass the flag: with it on, the old uninstaller would run first, without it.
        text = PACKAGING.read_text(encoding="utf-8")
        self.assertIn("set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)", text)
        self.assertNotIn("set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)", text)
        self.assertIn('set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${PROJECT_NAME}")', text, "the key the step reads")

    def test_it_is_the_nsis_generators_only(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        step = text.index("set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS")
        block = text.rindex('if(CPACK_GENERATOR STREQUAL "NSIS")', 0, step)
        self.assertNotIn("endif()", text[block:step])


if __name__ == "__main__":
    unittest.main()
