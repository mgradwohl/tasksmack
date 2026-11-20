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

#4436: the step first has TaskSmackInUseCheck look at the install folder and the installed copy's folder, before the
old uninstaller runs, and reads that uninstaller's exit code. The check itself is test_nsis_in_use.py's; here a
folder in `busy` makes it exit with TASKSMACK_IN_USE_EXIT_CODE, as a silent run does.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_nsis_ifeo_uninstall as ifeo  # noqa: E402
import test_nsis_run_uninstall as run  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
PACKAGING = REPO / "cmake" / "Packaging.cmake"
TRUST_SCRIPT = REPO / "cmake" / "nsis" / "TaskSmackUninstallerTrust.ps1"
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskSmack"
INSTDIR = r"C:\Program Files\TaskSmack"
UNINSTALLER = INSTDIR + r"\Uninstall.exe"
REGISTERS = {"$0": "zero", "$1": "one", "$2": "two"}
IN_USE_EXIT_CODE = 32
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

    def __init__(
        self,
        uninstall_string: str | None,
        instdir: str,
        files: set[str],
        can_start: bool = True,
        busy: set[str] | None = None,
        exit_code: int = 0,
        untrusted: set[str] | None = None,
    ) -> None:
        self.uninstall_string = uninstall_string
        self.files = {name.lower() for name in files}
        self.can_start = can_start
        self.busy = {name.lower() for name in busy or set()}
        self.exit_code = exit_code
        self.untrusted = {name.lower() for name in untrusted or set()}
        self.vars = {"$INSTDIR": instdir, **REGISTERS, "${TASKSMACK_IN_USE_EXIT_CODE}": str(IN_USE_EXIT_CODE)}
        self.stack: list[str] = []
        self.error = False
        self.executed: list[str] = []
        self.out_paths: list[str] = []
        self.messages: list[str] = []
        self.aborted = False
        self.checked: list[str] = []
        self.error_level: int | None = None
        self.quit = False
        self.trust_checks: list[tuple[str, str]] = []  # (mode, path) TaskSmackTrustCheck was asked
        self.removed_installs: list[str] = []  # the folders TaskSmackRemoveOldInstall cleared
        self.details: list[str] = []
        self.deleted: list[str] = []
        self.removed_dirs: list[str] = []

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
                self.details.append(self.expand(args[0]))
            elif op == "Delete":
                path = self.expand(args[0])
                self.files.discard(path.lower())
                self.deleted.append(path)
            elif op == "RMDir":
                path = self.expand(args[0])
                if not any(name.startswith(path.lower() + "\\") for name in self.files):  # NSIS removes an empty folder only
                    self.removed_dirs.append(path)
            elif op == "ExecWait":
                self.executed.append(self.expand(args[0]))
                if not self.can_start:
                    self.error = True
                elif len(args) > 1:
                    self.vars[args[1]] = str(self.exit_code)
            elif op == "Call" and args[0] == "TaskSmackTrustCheck":
                mode, path = self.stack.pop(), self.stack.pop()  # the path is pushed first, then the mode
                self.trust_checks.append((mode, path))
                self.stack.append("0" if path.lower() in self.untrusted else "1")
            elif op == "Call" and args[0] == "TaskSmackRemoveOldInstall":
                folder = self.stack.pop()
                self.removed_installs.append(folder)
                self.files = {name for name in self.files if not name.startswith(folder.lower() + "\\")}
            elif op == "Call":
                if args[0] != "TaskSmackInUseCheck":
                    raise AssertionError(f"the step calls {args[0]}, not TaskSmackInUseCheck")
                folder = self.stack.pop()
                self.checked.append(folder)
                if folder.lower() in self.busy:  # what the check does under /S: exit, before anything changes
                    self.error_level, self.quit = IN_USE_EXIT_CODE, True
                    return
            elif op == "SetErrorLevel":
                self.error_level = int(self.expand(args[0]))
            elif op == "Quit":
                self.quit = True
                return
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


def install(
    uninstall_string: str | None,
    instdir: str = INSTDIR,
    files: set[str] | None = None,
    can_start: bool = True,
    busy: set[str] | None = None,
    exit_code: int = 0,
    untrusted: set[str] | None = None,
) -> Installer:
    installer = Installer(
        uninstall_string, instdir, {UNINSTALLER} if files is None else files, can_start, busy, exit_code, untrusted
    )
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

        # #4445: its _?= run can't delete itself, so the installer deletes the old Uninstall.exe and the empty folder.
        self.assertEqual(installer.deleted, [UNINSTALLER])
        self.assertEqual(installer.removed_dirs, [INSTDIR])

    def test_a_move_leaves_the_old_folder_when_the_old_uninstaller_fails(self) -> None:
        installer = install(f'"{UNINSTALLER}"', instdir=r"D:\Apps\TaskSmack", exit_code=2)
        self.assertTrue(installer.aborted)
        self.assertEqual((installer.deleted, installer.removed_dirs), ([], []))

    def test_a_move_keeps_a_folder_that_still_holds_files(self) -> None:
        files = {UNINSTALLER, INSTDIR + r"\notes.txt"}
        installer = install(f'"{UNINSTALLER}"', instdir=r"D:\Apps\TaskSmack", files=files)
        self.assertEqual(installer.deleted, [UNINSTALLER])
        self.assertEqual(installer.removed_dirs, [], "RMDir without /r leaves a folder that isn't empty")

    def test_an_upgrade_in_place_deletes_nothing(self) -> None:
        # The new files are about to go there: the old Uninstall.exe is the new one's to overwrite.
        installer = install(f'"{UNINSTALLER}"')
        self.assertEqual((installer.deleted, installer.removed_dirs), ([], []))

    def test_an_upgrade_stays_in_a_custom_folder(self) -> None:
        # #4445: with no /D=, InstallDirRegKey makes $INSTDIR the folder the last install recorded. The step then sees
        # the same folder, so the old uninstaller keeps taskmgr.exe's Debugger and the Run value, and nothing is moved.
        custom = r"D:\Apps\TaskSmack"
        uninstaller = custom + r"\Uninstall.exe"
        installer = install(f'"{uninstaller}"', instdir=custom, files={uninstaller})
        self.assertEqual(installer.executed, [f'"{uninstaller}" /S /TASKSMACK_UPGRADE _?={custom}'])
        self.assertEqual(installer.out_paths, [custom])
        self.assertEqual((installer.deleted, installer.removed_dirs), ([], []))
        machine = Machine(custom)
        machine.uninstall(installer.executed[0])
        self.assertEqual(machine.debugger(), ("REG_SZ", f'"{custom}\\TaskSmack.exe"'))
        self.assertEqual(machine.run_entry(), machine.run_value)
        self.assertEqual(machine.mark, {})

    def test_the_default_install_folder_comes_from_the_registry(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn('InstallDirRegKey HKLM "Software\\TaskSmack Contributors\\TaskSmack" ""', text)
        # The key the template's WriteRegStr SHCTX writes, which InstallDirRegKey reads back.
        self.assertIn('set(CPACK_PACKAGE_VENDOR "TaskSmack Contributors")', PACKAGING.read_text(encoding="utf-8"))

    def test_an_unsafe_old_uninstaller_is_never_run(self) -> None:
        # #4444: an unsigned Uninstall.exe in a folder users can write to. Whatever folder the new copy goes to, and
        # however the upgrade got there, it isn't started: the install removes the old one by its own list instead.
        unsafe = r"D:\Shared\TaskSmack"
        uninstaller = unsafe + r"\Uninstall.exe"
        for instdir in (unsafe, INSTDIR):
            with self.subTest(instdir=instdir):
                installer = install(f'"{uninstaller}"', instdir=instdir, files={uninstaller}, untrusted={uninstaller})
                self.assertIn(("Uninstaller", uninstaller), installer.trust_checks)
                self.assertEqual(installer.executed, [], "never run")
                self.assertEqual(installer.removed_installs, [unsafe])
                self.assertFalse(installer.aborted)
                self.assertFalse(installer.quit)
                self.assertEqual(installer.out_paths, [instdir], "the install goes on")
                self.assertTrue(any("isn't run" in line for line in installer.details), "the log says why")
                self.assert_registers_back(installer)

    def test_every_upgrade_asks_before_it_runs_the_old_uninstaller(self) -> None:
        for instdir in (INSTDIR, r"D:\Apps\TaskSmack"):
            with self.subTest(instdir=instdir):
                installer = install(f'"{UNINSTALLER}"', instdir=instdir)
                self.assertEqual(installer.trust_checks[-1], ("Uninstaller", UNINSTALLER))
                self.assertEqual(len(installer.executed), 1)
                self.assertEqual(installer.removed_installs, [])
        installer = install(None)
        self.assertEqual(installer.trust_checks, [("Folder", INSTDIR)], "a first install only checks its folder")

    def test_an_install_folder_that_isnt_admin_only_is_logged(self) -> None:
        custom = r"D:\Apps\TaskSmack"
        warned = install(f'"{UNINSTALLER}"', instdir=custom, untrusted={custom})
        self.assertTrue(any(line.startswith("Warning:") and custom in line for line in warned.details))
        self.assertEqual(len(warned.executed), 1, "a warning only: the trusted old uninstaller still runs")
        quiet = install(f'"{UNINSTALLER}"')
        self.assertFalse(any(line.startswith("Warning:") for line in quiet.details))

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

    def test_both_folders_are_checked_before_the_old_uninstaller_runs(self) -> None:
        # #4436: the install folder, then the installed copy's folder, each before anything changes.
        installer = install(f'"{UNINSTALLER}"')
        self.assertEqual(installer.checked, [INSTDIR, INSTDIR])
        elsewhere = r"D:\Apps\TaskSmack"
        installer = install(f'"{UNINSTALLER}"', instdir=elsewhere)
        self.assertEqual(installer.checked, [elsewhere, INSTDIR])
        installer = install(None)
        self.assertEqual(installer.checked, [INSTDIR], "a first install checks the folder it writes to")
        self.assertFalse(installer.quit)
        self.assert_registers_back(installer)

    def test_a_running_tasksmack_stops_the_install_before_the_uninstaller(self) -> None:
        elsewhere = r"D:\Apps\TaskSmack"
        for instdir, busy in ((INSTDIR, INSTDIR), (elsewhere, INSTDIR), (elsewhere, elsewhere)):
            with self.subTest(instdir=instdir, busy=busy):
                installer = install(f'"{UNINSTALLER}"', instdir=instdir, busy={busy})
                self.assertTrue(installer.quit)
                self.assertEqual(installer.error_level, IN_USE_EXIT_CODE)
                self.assertEqual(installer.executed, [], "the old uninstaller never runs")
                self.assertEqual(installer.out_paths, [])

    def test_an_uninstaller_that_finds_tasksmack_running_stops_the_install_with_its_code(self) -> None:
        installer = install(f'"{UNINSTALLER}"', exit_code=IN_USE_EXIT_CODE)
        self.assertEqual(len(installer.executed), 1)
        self.assertTrue(installer.quit)
        self.assertFalse(installer.aborted)
        self.assertEqual(installer.error_level, IN_USE_EXIT_CODE)
        self.assertEqual(installer.out_paths, [], "nothing more is installed")

    def test_an_uninstaller_that_fails_stops_the_install(self) -> None:
        installer = install(f'"{UNINSTALLER}"', exit_code=2)
        self.assertEqual(installer.messages, ["Uninstall failed (exit code 2)."])
        self.assertTrue(installer.aborted)
        self.assertEqual(installer.out_paths, [])

    def test_it_is_embedded_and_run_without_a_profile_or_window(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn('File "/oname=$PLUGINSDIR\\TaskSmackUninstallerTrust.ps1" "${TASKSMACK_TRUST_SCRIPT}"', text)
        self.assertIn("-NoProfile -NonInteractive -ExecutionPolicy Bypass -File", text)
        self.assertIn("cmake/nsis/TaskSmackUninstallerTrust.ps1", text)
        self.assertTrue(TRUST_SCRIPT.is_file())

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


def windows_powershell() -> str | None:
    """Windows PowerShell 5.1, which the installer runs the script in (nsExec, $SYSDIR); only on Windows."""
    if sys.platform != "win32":
        return None
    path = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    return str(path) if path.is_file() else None


@unittest.skipUnless(windows_powershell(), "the trust script runs in Windows PowerShell")
class TrustScriptTest(unittest.TestCase):
    """cmake/nsis/TaskSmackUninstallerTrust.ps1 (#4444), run for real on temporary folders. It only reads ACLs
    (Remove mode deletes what it is given), needs no elevation and installs nothing."""

    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="tasksmack-trust-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def script(self, mode: str, path: Path | str) -> subprocess.CompletedProcess[str]:
        command = [windows_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File"]
        command += [str(TRUST_SCRIPT), "-Mode", mode, "-Path", str(path), "-Signer", "Matt Gradwohl"]
        return subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)

    def users_writable(self, name: str) -> Path:
        """A folder with a fake, unsigned Uninstall.exe that any user (BUILTIN\\Users) may change."""
        folder = self.root / name
        folder.mkdir()
        (folder / "Uninstall.exe").write_bytes(b"MZ not a real program")
        grant = subprocess.run(["icacls", str(folder), "/grant", "*S-1-5-32-545:(OI)(CI)M"], capture_output=True, text=True, check=False)
        self.assertEqual(grant.returncode, 0, grant.stdout + grant.stderr)
        return folder

    def test_an_unsigned_uninstaller_in_a_users_writable_folder_isnt_trusted(self) -> None:
        folder = self.users_writable("TaskSmack")
        result = self.script("Uninstaller", folder / "Uninstall.exe")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("is not trusted", result.stdout)
        warned = self.script("Folder", folder)
        self.assertEqual(warned.returncode, 1, warned.stdout + warned.stderr)

    def test_a_missing_uninstaller_isnt_trusted(self) -> None:
        result = self.script("Uninstaller", self.root / "nothing" / "Uninstall.exe")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_a_file_only_administrators_can_change_is_trusted(self) -> None:
        # %SystemRoot%\System32 is owned by TrustedInstaller, and only SYSTEM, Administrators and TrustedInstaller may change it.
        system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
        result = self.script("Uninstaller", system / "cmd.exe")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.script("Folder", system).returncode, 0)

    def test_the_folder_check_looks_above_a_folder_that_isnt_there_yet(self) -> None:
        folder = self.users_writable("Fresh")
        self.assertEqual(self.script("Folder", folder / "not" / "made" / "yet").returncode, 1)

    def test_remove_deletes_the_install_by_the_list_and_never_follows_a_junction(self) -> None:
        folder = self.root / "TaskSmack"
        outside = self.root / "outside"
        (folder / "share" / "tasksmack" / "assets").mkdir(parents=True)
        outside.mkdir()
        (outside / "keep.txt").write_text("not TaskSmack's", encoding="utf-8")
        for name in ("TaskSmack.exe", "TaskSmackHelper.exe", "LICENSE.txt", "Uninstall.exe", "notes.txt"):
            (folder / name).write_text("x", encoding="utf-8")
        (folder / "share" / "tasksmack" / "assets" / "font.ttf").write_text("x", encoding="utf-8")
        link = subprocess.run(["cmd", "/c", "mklink", "/J", str(folder / "third_party_licenses"), str(outside)], capture_output=True, text=True, check=False)
        self.assertEqual(link.returncode, 0, link.stdout + link.stderr)
        result = self.script("Remove", folder)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["notes.txt"], "only its own files go, the junction too")
        self.assertEqual((outside / "keep.txt").read_text(encoding="utf-8"), "not TaskSmack's", "the junction's target is untouched")

    def test_remove_takes_an_emptied_folder_too(self) -> None:
        folder = self.root / "TaskSmack"
        folder.mkdir()
        (folder / "Uninstall.exe").write_text("x", encoding="utf-8")
        self.assertEqual(self.script("Remove", folder).returncode, 0)
        self.assertFalse(folder.exists())
        self.assertEqual(self.script("Remove", folder).returncode, 0, "a folder that is gone is fine")


if __name__ == "__main__":
    unittest.main()
