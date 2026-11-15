#!/usr/bin/env python3
"""Tests for find_python (#2411), list_format_files and coverage_ignore_regex (#3528, #3911) in tools/common.sh.

A checkout shared between Windows and WSL can hold the Windows venv setup-dev.ps1 creates
(.venv/Scripts/python.exe, no .venv/bin/python). Only a Windows bash (Git Bash/MSYS2, Cygwin) may
pick that interpreter; on Linux, WSL included, find_python must skip it, since a Windows python.exe
run through interop reads Linux paths as C:\\mnt\\.... Each case sources a copy of common.sh in a
temporary checkout and sets OSTYPE before calling find_python, so one host covers both kinds of
bash. Needs bash; registered in CTest only where bash is available.
"""

import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COMMON_SH = REPO_ROOT / "tools" / "common.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

# Stands in for a venv interpreter: reports Python 3.14 to _python_meets_min's version probe.
STUB = "#!/bin/sh\necho 3.14\n"


def posix(path: Path) -> str:
    """A path bash accepts on every platform (forward slashes; Git Bash maps C:/ itself)."""
    return str(path).replace("\\", "/")


class FindPythonTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.checkout = Path(self._tmp.name) / "checkout"
        (self.checkout / "tools").mkdir(parents=True)
        shutil.copy2(COMMON_SH, self.checkout / "tools" / "common.sh")

    def tearDown(self):
        self._tmp.cleanup()

    def add_venv_python(self, rel: str) -> Path:
        path = self.checkout / ".venv" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(STUB, encoding="utf-8", newline="\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return path

    def find_python(self, ostype: str) -> subprocess.CompletedProcess:
        script = 'source "$1"; OSTYPE="$2"; find_python'
        return subprocess.run(
            [BASH, "-c", script, "test", posix(self.checkout / "tools" / "common.sh"), ostype],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_a_windows_venv_is_skipped_on_linux(self):
        # #2411: in WSL, find_python falls back to PATH (or fails) rather than return the shared
        # checkout's Windows interpreter.
        stub = self.add_venv_python("Scripts/python.exe")
        for ostype in ("linux-gnu", "linux-musl", "darwin23"):
            with self.subTest(ostype=ostype):
                result = self.find_python(ostype)
                self.assertNotIn(posix(stub), result.stdout, result.stderr)
                self.assertNotIn("Scripts/python.exe", result.stdout, result.stderr)

    def test_a_windows_venv_is_used_on_a_windows_bash(self):
        # #2276: from Git Bash (msys) or Cygwin, setup-dev.ps1's venv comes before PATH.
        self.add_venv_python("Scripts/python.exe")
        for ostype in ("msys", "cygwin"):
            with self.subTest(ostype=ostype):
                result = self.find_python(ostype)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip().endswith("/.venv/Scripts/python.exe"), result.stdout)

    def test_a_posix_venv_is_used_on_every_bash(self):
        # The POSIX venv comes first everywhere, ahead of a Windows one beside it.
        self.add_venv_python("bin/python")
        self.add_venv_python("Scripts/python.exe")
        for ostype in ("linux-gnu", "msys", "cygwin"):
            with self.subTest(ostype=ostype):
                result = self.find_python(ostype)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip().endswith("/.venv/bin/python"), result.stdout)

    def test_a_native_interpreters_crlf_version_is_accepted(self):
        # #3517: Windows Python prints CRLF, and MSYS2/Cygwin bash builds without Git for Windows'
        # patch keep the CR in $(...). The trailing " \r" survives even Git Bash's CRLF handling.
        path = self.checkout / ".venv" / "Scripts" / "python.exe"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nprintf '3.14\\r \\r\\n'\n", encoding="utf-8", newline="\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        for ostype in ("msys", "cygwin"):
            with self.subTest(ostype=ostype):
                result = self.find_python(ostype)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip().endswith("/.venv/Scripts/python.exe"), result.stdout)

    def test_strip_native_output_drops_crs_and_surrounding_whitespace(self):
        script = 'source "$1"; printf "[%s]" "$(strip_native_output "$2")"'
        cases = {"win32\r\n": "[win32]", "win32\r \r": "[win32]", " C:\\a b\\c\r": "[C:\\a b\\c]", "": "[]"}
        for value, expected in cases.items():
            with self.subTest(value=value):
                result = subprocess.run(
                    [BASH, "-c", script, "test", posix(self.checkout / "tools" / "common.sh"), value],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)


class CheckoutPathTests(unittest.TestCase):
    """#3911: the Linux twins of #3528's fixes, as tools/test-format-sources.ps1 checks the Windows ones. A checkout
    under folders named build and tests, with a dot, spaces and parentheses in its name, reached through a link."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name)
        self.root = base / "build" / "tests" / "task.smack (x86)"
        for rel in ("src/a.cpp", "src/build/gen.h", "tests/t.cpp", "benchmarks/b.cpp", "build/debug/out.h"):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        self.link = base / "link"
        try:
            self.link.symlink_to(self.root, target_is_directory=True)
        except OSError:
            self.link = None

    def tearDown(self):
        self._tmp.cleanup()

    def call(self, function: str, root: str) -> str:
        script = 'source "$1"; "$2" "$3"'
        result = subprocess.run(
            [BASH, "-c", script, "test", posix(COMMON_SH), function, root], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_list_format_files_finds_the_sources_whatever_the_checkout_is_under(self):
        root = posix(self.root)
        listed = sorted(name for name in self.call("list_format_files", root).split("\0") if name)
        self.assertEqual(listed, sorted(f"{root}/{rel}" for rel in ("src/a.cpp", "tests/t.cpp", "benchmarks/b.cpp")))

    def test_coverage_ignore_regex_is_anchored_at_the_checkout_and_its_physical_path(self):
        if os.name == "nt":
            # Only tools/coverage.sh calls it, which runs on Linux; Git Bash's pwd -P spells the physical path
            # /c/..., not Python's C:/..., so the two forms this test compares don't meet there.
            self.skipTest("coverage_ignore_regex serves tools/coverage.sh, which is Linux-only")
        if self.link is None:
            self.skipTest("can't make a directory link here")
        given = posix(self.link)
        physical = posix(self.root.resolve())
        regex = re.compile(self.call("coverage_ignore_regex", given))
        for root in (given, physical):
            with self.subTest(root=root):
                for rel in ("tests/X.cpp", "build/Y.h", "benchmarks/B.cpp"):
                    self.assertTrue(regex.search(f"{root}/{rel}"), rel)
                for rel in ("src/build/G.h", "src/a.cpp", "src/tests/T.h"):
                    self.assertFalse(regex.search(f"{root}/{rel}"), rel)
        self.assertTrue(regex.search("/anywhere/_deps/sdl/Z.h"))
        self.assertTrue(regex.search(f"{given}/.cache/fetchcontent/x.h"))


if __name__ == "__main__":
    unittest.main()
