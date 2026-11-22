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


class RootTestFilterTests(unittest.TestCase):
    """#3836: CI's PR root step and tools/coverage.sh's root pass run the same root tests, from one list."""

    SUITES = (
        "DetachedSpawnTest.AsRoot*",
        "*/UdpByteCounterRootTest.*",
        "UdpTrafficCollectorRootTest.*",
        "SessionUserRootTest.*",
        "RootCapabilitiesRootTest.*",
        "LinuxProcessActionsRootTest.*",
        "RootDumpFolderRootTest.*",  # rootDumpRefusal() fails closed on a home under /tmp (#4262)
        "RootEnvironmentRootTest.*",  # the real openAsUser() as root (#4255)
        "UserConfigRootTest.*",  # the run-as-root-for-a-user config save hands the file to the folder's owner (#4549)
        "NetlinkSocketStatsRootTest.*",  # a datagram from another socket is never read as the kernel's (#4387)
        "LinuxHeldFolderRootTest.*",  # root for a user's held config folder gives what it makes to the owner (#4167)
    )

    def test_the_filter_names_every_root_suite(self):
        result = subprocess.run(
            [BASH, "-c", 'source "$1"; root_test_filter', "test", posix(COMMON_SH)], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().split(":"), list(self.SUITES))

    def test_ci_and_coverage_take_it_rather_than_a_copy(self):
        repo = REPO_ROOT
        for path in (repo / "tools" / "coverage.sh", repo / ".github" / "workflows" / "reusable-build-test.yml"):
            with self.subTest(path=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertIn("root_test_filter", text)
                self.assertNotIn("RootCapabilitiesRootTest.*:", text, "a hand-kept copy of the filter")


class PinnedLlvmMajorTests(unittest.TestCase):
    """#3824: the hooks are pinned by commit, with the tag in a `# frozen:` comment; the LLVM major still reads."""

    def test_the_major_comes_from_the_frozen_clang_format_tag(self):
        config = (REPO_ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        match = re.search(r"mirrors-clang-format\s+rev: [0-9a-f]{40} # frozen: v(\d+)\.", config)
        self.assertIsNotNone(match, "mirrors-clang-format is pinned by commit, with its tag frozen beside it")
        result = subprocess.run(
            [BASH, "-c", 'source "$1"; pinned_llvm_major', "test", posix(COMMON_SH)], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), match.group(1))


@unittest.skipIf(os.name == "nt", "the stub LLVM tools are POSIX shell scripts")
class CoveragePrereqsTests(unittest.TestCase):
    """#4711: llvm-cov and llvm-profdata are found as coverage.sh runs them (the LLVM folders first) and held to 23."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "lib"

    def tearDown(self):
        self._tmp.cleanup()

    def stub_llvm(self, major, version):
        folder = self.root / f"llvm-{major}" / "bin"
        folder.mkdir(parents=True, exist_ok=True)
        for tool in ("llvm-cov", "llvm-profdata"):
            path = folder / tool
            path.write_text(f"#!/bin/sh\necho 'Ubuntu LLVM version {version}'\n", encoding="utf-8", newline="\n")
            path.chmod(0o755)

    def validate(self):
        env = dict(os.environ, TASKSMACK_LLVM_LIB_ROOT=posix(self.root))
        return subprocess.run(
            [BASH, "-c", 'source "$1"; validate_coverage_prereqs', "test", posix(COMMON_SH)],
            capture_output=True, text=True, check=False, env=env,
        )

    def test_the_llvm_folders_tools_are_enough_without_unversioned_names(self):
        self.stub_llvm(23, "23.1.5")  # a --minimal setup: llvm-23 installed, no alternatives registered
        result = self.validate()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_an_llvm_older_than_the_floor_is_refused_naming_the_versioned_package(self):
        self.stub_llvm(22, "22.1.2")
        result = self.validate()
        self.assertEqual(result.returncode, 1)
        self.assertIn(">= 23 required, found: 22", result.stderr)
        self.assertIn("apt install llvm-23", result.stderr)


if __name__ == "__main__":
    unittest.main()
