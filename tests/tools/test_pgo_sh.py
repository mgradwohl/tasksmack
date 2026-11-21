#!/usr/bin/env python3
"""tools/pgo.sh's help and an unknown action (#4216): help asked for exits 0, as the other scripts' -h does
(#2276); an action it doesn't know prints the usage and exits 1. And resolve_pgo_profdata() (#4226), which must take
llvm-profdata from LLVM 23 only: a profile merged by another LLVM is refused by clang 23, or silently ignored. None of
it builds anything. Linux only."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"


@unittest.skipUnless(sys.platform.startswith("linux"), "pgo.sh is Linux-only")
class PgoShTest(unittest.TestCase):
    def run_pgo(self, *args):
        return subprocess.run([BASH, str(REPO_ROOT / "tools" / "pgo.sh"), *args], capture_output=True, text=True, check=False, timeout=60)

    def test_help_exits_zero_with_the_usage(self):
        for flag in ("-h", "--help", "help"):
            with self.subTest(flag=flag):
                result = self.run_pgo(flag)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Usage:", result.stdout)

    def test_an_unknown_action_prints_the_usage_and_fails(self):
        result = self.run_pgo("bogus")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Usage:", result.stdout)


# An llvm-profdata that says it is LLVM <major>, as `llvm-profdata --version` does.
STUB_PROFDATA = """#!/bin/sh
echo "LLVM (http://llvm.org/):"
echo "  LLVM version {major}.1.0"
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "pgo.sh is Linux-only")
class ResolvePgoProfdataTest(unittest.TestCase):
    """resolve_pgo_profdata(), lifted out of pgo.sh and run on a PATH holding only the stubs a case puts there (and
    grep and sed), with the Debian path /usr/lib/llvm-23 moved into the case's folder, so the machine's own LLVM
    can't answer."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="pgo_profdata_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in ("grep", "sed"):
            os.symlink(shutil.which(tool), self.bin / tool)
        script = (REPO_ROOT / "tools" / "pgo.sh").read_text(encoding="utf-8")
        match = re.search(r"^resolve_pgo_profdata\(\) \{\n.*?^\}\n", script, re.S | re.M)
        self.assertIsNotNone(match, "resolve_pgo_profdata() is in pgo.sh")
        self.debian = self.root / "usr-lib-llvm-23"
        self.function = match.group(0).replace("/usr/lib/llvm-23", str(self.debian))

    def stub(self, path: Path, major: int):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(STUB_PROFDATA.format(major=major), encoding="utf-8")
        path.chmod(0o755)

    def resolve(self):
        return subprocess.run(
            [BASH, "-c", self.function + "resolve_pgo_profdata"],
            capture_output=True, text=True, check=False, timeout=60, env={"PATH": str(self.bin)},
        )

    def test_an_unversioned_llvm_profdata_is_taken_only_when_it_is_llvm_23(self):
        self.stub(self.bin / "llvm-profdata", 23)
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.bin / "llvm-profdata"))
        self.stub(self.bin / "llvm-profdata", 22)
        result = self.resolve()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("llvm-profdata from LLVM 23 not found", result.stderr)

    def test_the_debian_path_then_the_versioned_name_come_first(self):
        self.stub(self.bin / "llvm-profdata", 23)
        self.stub(self.bin / "llvm-profdata-23", 23)
        self.assertEqual(self.resolve().stdout.strip(), str(self.bin / "llvm-profdata-23"))
        self.stub(self.debian / "bin" / "llvm-profdata", 23)
        self.assertEqual(self.resolve().stdout.strip(), str(self.debian / "bin" / "llvm-profdata"))

    def test_nothing_at_all_says_how_to_install_it(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("sudo apt install llvm-23", result.stderr)


if __name__ == "__main__":
    unittest.main()
