#!/usr/bin/env python3
"""Tests for the installer's and the uninstaller's check for a running TaskSmack (#4436), in
cmake/CPackProjectConfig.cmake.in.

An upgrade or an uninstall while TaskSmack.exe or TaskSmackHelper.exe runs used to leave a half-applied install: the
locked .exe was skipped (NSIS's AllowSkipFiles) and everything around it replaced or removed, and a silent install
still exited 0. TaskSmackInUseCheck (un.TaskSmackInUseCheck in the uninstaller), defined in CPACK_NSIS_DEFINES, takes a
folder on the stack and opens each of the two .exe files there for writing, which Windows refuses for a running
program in any session. In use, it asks the user to close TaskSmack (Retry/Cancel); Cancel, and a silent run at once,
exits with TASKSMACK_IN_USE_EXIT_CODE before anything changes. The installer calls it before anything is written
(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS) and the uninstaller before anything is removed (the first lines of
CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS). WinGet's installer manifest declares that code as packageInUse.

makensis isn't on every machine, and an installer can't be run in a test, so the function's lines are read out of the
file and run here by a small interpreter of the NSIS commands they use, against a fake folder. When a project.nsi from
a real CPack run is named in TASKSMACK_NSIS_PROJECT, it is checked too.
"""

import os
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_nsis_ifeo_uninstall as ifeo  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
WINGET_INSTALLER = REPO / "packaging" / "winget" / "TaskSmack.TaskSmack.installer.yaml"
CHECKER = REPO / "tools" / "check-winget-manifests.ps1"
FOLDER = r"C:\Program Files\TaskSmack"
EXES = ("TaskSmack.exe", "TaskSmackHelper.exe")
REGISTERS = {"$0": "zero", "$1": "one", "$2": "two"}
BEGIN = "; TaskSmack in use (#4436): BEGIN"
END = "; TaskSmack in use (#4436): END"


def defines_block() -> list[str]:
    """The check's lines in CPACK_NSIS_DEFINES, from its BEGIN marker to its END marker."""
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(r"set\(CPACK_NSIS_DEFINES \[=\[\n(.*?)\]=\]\)", text, re.DOTALL)
    if match is None:
        raise AssertionError("no CPACK_NSIS_DEFINES in " + str(CONFIG))
    lines = match.group(1).splitlines()
    stripped = [line.strip() for line in lines]
    if BEGIN not in stripped or END not in stripped:
        raise AssertionError("the check's BEGIN and END markers are missing from CPACK_NSIS_DEFINES")
    return lines[stripped.index(BEGIN) : stripped.index(END) + 1]


def exit_code() -> int:
    for line in defines_block():
        words = ifeo.tokens(line)
        if words[:2] == ["!define", "TASKSMACK_IN_USE_EXIT_CODE"]:
            return int(words[2])
    raise AssertionError("no !define TASKSMACK_IN_USE_EXIT_CODE")


def function_lines(prefix: str) -> list[str]:
    """The function's body as the macro inserts it for prefix ("" or "un."), Function and FunctionEnd left out."""
    block = defines_block()
    stripped = [line.strip() for line in block]
    start = stripped.index("!macro TASKSMACK_IN_USE_CHECK UN")
    end = stripped.index("!macroend")
    body = [line.replace("${UN}", prefix) for line in block[start + 1 : end]]
    if body[0].strip() != f"Function {prefix}TaskSmackInUseCheck" or body[-1].strip() != "FunctionEnd":
        raise AssertionError(f"the macro doesn't define one function, {prefix}TaskSmackInUseCheck: {body[0]!r} ... {body[-1]!r}")
    return body[1:-1]


class Answers:
    """The user's answers to the message box, in order; each may close TaskSmack first (Retry after closing it)."""

    def __init__(self, *answers: tuple[str, bool]) -> None:
        self.answers = list(answers)


class Check:
    """Runs the function's lines as NSIS would: registers, a stack, the error flag, and a folder whose files may be
    in use (open for writing refused). FileOpen creates a missing file, as NSIS's "a" mode does."""

    def __init__(self, files: dict[str, bool], silent: bool = True, answers: Answers | None = None) -> None:
        self.files = {name.lower(): busy for name, busy in files.items()}
        self.silent = silent
        self.answers = answers.answers if answers else []
        self.vars = dict(REGISTERS)
        self.vars["${TASKSMACK_IN_USE_EXIT_CODE}"] = str(exit_code())
        self.stack: list[str] = []
        self.error = False
        self.open_handles: set[str] = set()
        self.created: list[str] = []
        self.messages: list[str] = []
        self.error_level: int | None = None
        self.quit = False

    def expand(self, word: str) -> str:
        for name in sorted(self.vars, key=len, reverse=True):
            word = word.replace(name, self.vars[name])
        return word

    def call(self, folder: str, prefix: str = "") -> None:
        self.stack.append(folder)
        self.run(function_lines(prefix))

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
                raise AssertionError("the check doesn't end")
            words = program[pc]
            if not words or (len(words) == 1 and words[0].endswith(":")):
                pc += 1
                continue
            op, args = words[0], words[1:]
            nxt = pc + 1
            if op == "Exch":
                top = self.stack.pop()
                self.stack.append(self.vars[args[0]])
                self.vars[args[0]] = top
            elif op == "Push":
                self.stack.append(self.expand(args[0]))
            elif op == "Pop":
                self.vars[args[0]] = self.stack.pop()
            elif op == "StrCpy":
                if len(args) != 2:
                    raise AssertionError(f"the interpreter only copies whole strings: {words}")
                self.vars[args[0]] = self.expand(args[1])
            elif op == "StrCmp":
                same = self.expand(args[0]).lower() == self.expand(args[1]).lower()
                nxt = jump(args[2]) if same else jump(args[3] if len(args) > 3 else "")
            elif op == "IfFileExists":
                exists = self.expand(args[0]).lower() in self.files
                nxt = jump(args[1]) if exists else jump(args[2] if len(args) > 2 else "")
            elif op == "ClearErrors":
                self.error = False
            elif op == "FileOpen":
                handle, path, mode = args[0], self.expand(args[1]).lower(), args[2]
                if mode != "a":
                    raise AssertionError(f'FileOpen mode {mode}: only "a" opens for writing without truncating')
                if path not in self.files:
                    self.files[path] = False
                    self.created.append(path)
                if self.files[path]:
                    self.error = True
                else:
                    self.vars[handle] = "handle:" + path
                    self.open_handles.add(path)
            elif op == "FileClose":
                self.open_handles.discard(self.expand(args[0]).removeprefix("handle:"))
            elif op == "IfErrors":
                nxt = jump(args[0]) if self.error else jump(args[1] if len(args) > 1 else "")
            elif op == "Goto":
                nxt = jump(args[0])
            elif op == "DetailPrint":
                pass
            elif op == "MessageBox":
                if "/SD" not in args:
                    raise AssertionError("a message box needs /SD, for a silent install")
                self.messages.append(self.expand(args[1]))
                sd = args.index("/SD")
                checks = dict(zip(args[sd + 2 :: 2], args[sd + 3 :: 2]))
                if self.silent:
                    answer = args[sd + 1]
                else:
                    answer, closes = self.answers.pop(0)
                    if closes:
                        self.files = {path: False for path in self.files}
                if answer in checks:
                    nxt = jump(checks[answer])
            elif op == "SetErrorLevel":
                self.error_level = int(self.expand(args[0]))
            elif op == "Quit":
                self.quit = True
                return
            else:
                raise AssertionError(f"the interpreter doesn't know {op}")
            pc = nxt


def files(busy: tuple[str, ...] = (), present: tuple[str, ...] = EXES) -> dict[str, bool]:
    return {f"{FOLDER}\\{name}": name in busy for name in present}


def bracket(variable: str) -> list[tuple[str, list[str]]]:
    """Each set/string(APPEND|PREPEND) of variable in the file, in order: (how, its lines)."""
    text = CONFIG.read_text(encoding="utf-8")
    pattern = rf"(set|string\(APPEND|string\(PREPEND)\(?\s*{variable} \[=\[\n(.*?)\]=\]\)"
    return [(m.group(1), m.group(2).splitlines()) for m in re.finditer(pattern, text, re.DOTALL)]


def composed(variable: str) -> list[str]:
    """The variable's lines as CMake builds them, running the file's set and string(APPEND|PREPEND) calls in order."""
    lines: list[str] = []
    for how, part in bracket(variable):
        if how == "set":
            lines = list(part)
        elif how == "string(APPEND":
            lines = lines + part
        else:
            lines = part + lines
    return lines


def commands(lines: list[str]) -> list[list[str]]:
    return [words for words in (ifeo.tokens(line) for line in lines) if words]


class InUseCheckTest(unittest.TestCase):
    def assert_back(self, check: Check) -> None:
        self.assertEqual({name: check.vars[name] for name in REGISTERS}, REGISTERS, "its registers are put back")
        self.assertEqual(check.stack, [])
        self.assertEqual(check.open_handles, set(), "every file it opened is closed")
        self.assertEqual(check.created, [], "it creates nothing")

    def test_both_prefixes_get_the_function(self) -> None:
        block = [line.strip() for line in defines_block()]
        self.assertIn('!insertmacro TASKSMACK_IN_USE_CHECK ""', block)
        self.assertIn('!insertmacro TASKSMACK_IN_USE_CHECK "un."', block)
        self.assertEqual(function_lines(""), [line.replace("un.", "") for line in function_lines("un.")])

    def test_nothing_running_returns(self) -> None:
        for prefix in ("", "un."):
            with self.subTest(prefix=prefix):
                check = Check(files())
                check.call(FOLDER, prefix)
                self.assertFalse(check.quit)
                self.assertEqual(check.messages, [])
                self.assert_back(check)

    def test_a_missing_exe_is_not_created(self) -> None:
        # A build without the helper (unsigned), and a first install into an empty folder.
        for present in (("TaskSmack.exe",), ()):
            with self.subTest(present=present):
                check = Check(files(present=present))
                check.call(FOLDER)
                self.assertFalse(check.quit)
                self.assert_back(check)

    def test_a_running_exe_stops_a_silent_run_with_the_code(self) -> None:
        for busy in (("TaskSmack.exe",), ("TaskSmackHelper.exe",), EXES):
            for prefix in ("", "un."):
                with self.subTest(busy=busy, prefix=prefix):
                    check = Check(files(busy=busy))
                    check.call(FOLDER, prefix)
                    self.assertTrue(check.quit)
                    self.assertEqual(check.error_level, exit_code())
                    self.assertEqual(len(check.messages), 1)
                    self.assertEqual(check.created, [])

    def test_interactive_retry_after_closing_goes_on(self) -> None:
        check = Check(files(busy=("TaskSmack.exe",)), silent=False, answers=Answers(("IDRETRY", False), ("IDRETRY", True)))
        check.call(FOLDER, "un.")
        self.assertFalse(check.quit)
        self.assertEqual(len(check.messages), 2, "Retry while it still runs asks again")
        self.assertIn("Close TaskSmack", check.messages[0])
        self.assert_back(check)

    def test_interactive_cancel_exits_with_the_code(self) -> None:
        check = Check(files(busy=("TaskSmackHelper.exe",)), silent=False, answers=Answers(("IDCANCEL", False)))
        check.call(FOLDER)
        self.assertTrue(check.quit)
        self.assertEqual(check.error_level, exit_code())

    def test_the_code_is_32_and_winget_reads_it_as_package_in_use(self) -> None:
        code = exit_code()
        self.assertEqual(code, 32)
        self.assertNotIn(code, (0, 1, 2), "NSIS's own codes: success, cancelled by the user, aborted by the script")
        manifest = WINGET_INSTALLER.read_text(encoding="utf-8")
        self.assertRegex(manifest, rf"(?m)^ExpectedReturnCodes:\n  - InstallerReturnCode: {code}\n    ReturnResponse: packageInUse$")
        checker = CHECKER.read_text(encoding="utf-8")
        self.assertIn(f"$InUseExitCode = '{code}'", checker)
        self.assertIn("$InUseResponse = 'packageInUse'", checker)

    def test_the_uninstaller_checks_first(self) -> None:
        parts = bracket("CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS")
        self.assertEqual([how for how, _ in parts].count("string(PREPEND"), 1)
        self.assertEqual(parts[-1][0], "string(PREPEND", "prepended after every other block, so it stays first")
        first = commands(composed("CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS"))[:2]
        self.assertEqual(first, [["Push", "$INSTDIR"], ["Call", "un.TaskSmackInUseCheck"]])

    def test_the_installer_checks_before_it_writes_or_uninstalls(self) -> None:
        program = commands(composed("CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS"))
        self.assertEqual(program[:2], [["Push", "$INSTDIR"], ["Call", "TaskSmackInUseCheck"]])
        calls = [i for i, words in enumerate(program) if words == ["Call", "TaskSmackInUseCheck"]]  # not #4444's trust checks
        execs = [i for i, words in enumerate(program) if words[0] == "ExecWait"]
        self.assertEqual(len(calls), 2)
        self.assertTrue(execs and all(i > calls[1] for i in execs), "the installed copy's folder is checked before its uninstaller runs")
        self.assertEqual(program[calls[1] - 1], ["Push", "$1"])
        self.assertTrue(all(len(program[i]) == 3 for i in execs), "the old uninstaller's exit code is read")

    @unittest.skipUnless(os.environ.get("TASKSMACK_NSIS_PROJECT"), "no CPack project.nsi named")
    def test_generated_project_nsi(self) -> None:
        text = Path(os.environ["TASKSMACK_NSIS_PROJECT"]).read_text(encoding="utf-8-sig")
        self.assertIn("!define TASKSMACK_IN_USE_EXIT_CODE 32", text)
        uninstall = [line.strip() for line in text[text.index('Section "Uninstall"') :].splitlines()]
        call = uninstall.index("Call un.TaskSmackInUseCheck")
        self.assertEqual(uninstall[call - 1], "Push $INSTDIR")
        changes = [i for i, line in enumerate(uninstall) if line.startswith(("Delete", "RMDir", "WriteReg", "SetRegView"))]
        self.assertTrue(changes and call < changes[0], "the check comes before the uninstaller changes anything")
        install = [line.strip() for line in text[text.index('Section "-Core installation"') :].splitlines()]
        self.assertEqual(install[install.index("Call TaskSmackInUseCheck") - 1], "Push $INSTDIR")
        self.assertLess(install.index("Call TaskSmackInUseCheck"), min(i for i, line in enumerate(install) if line.startswith(("File ", "ExecWait"))))


if __name__ == "__main__":
    unittest.main()
