#!/usr/bin/env python3
"""Tests for tools/check-prereqs.sh's optional ccache row (#2350).

ccache is recommended, not required. Whatever a ccache on PATH prints (a two- or three-component
version, an old one, nothing it can parse, or a failure), and when there is none, the checker must
report it and go on to the remaining checks and the summary rather than exit under
set -euo pipefail. Each case runs the checker with PATH set to a stub ccache plus the few
utilities the script needs, so the host's own tools do not change the result. Needs bash;
registered in CTest only where bash is available.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECK_PREREQS_SH = REPO_ROOT / "tools" / "check-prereqs.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"
UTILITIES = ["awk", "cat", "cut", "dirname", "grep", "head", "sed", "sort", "tr", "uname"]
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SUMMARY = re.compile(
    r"^(All mandatory prerequisites are satisfied\.|Some mandatory prerequisites are missing or out of date\.)$",
    re.M,
)


class CheckPrereqsCcacheTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.bin = Path(self._tmp.name) / "bin"
        self.bin.mkdir()
        for name in UTILITIES:
            found = shutil.which(name)
            if found is None:
                self.skipTest(f"{name} not on PATH")
            (self.bin / name).symlink_to(found)

    def tearDown(self):
        self._tmp.cleanup()

    def stub_ccache(self, body):
        stub = self.bin / "ccache"
        stub.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
        stub.chmod(0o755)
        return stub

    def run_checker(self):
        env = {"PATH": str(self.bin), "HOME": self._tmp.name}
        result = subprocess.run(
            [BASH, str(CHECK_PREREQS_SH)], capture_output=True, text=True, env=env, check=False, timeout=60
        )
        out = ANSI.sub("", result.stdout + result.stderr)
        self.assertRegex(out, SUMMARY, f"no summary (exit {result.returncode}):\n{out}")
        # The checks after ccache ran too
        self.assertRegex(out, r"(?m)^git: ", out)
        self.assertLess(out.index("ccache"), out.index("\ngit: "), out)
        return out

    def ccache_row(self, out):
        rows = [line for line in out.splitlines() if line.startswith("ccache: ")]
        self.assertEqual(len(rows), 1, out)
        return rows[0]

    def test_two_component_version(self):
        stub = self.stub_ccache('echo "ccache version 4.14"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.14 ({stub})")

    def test_three_component_version(self):
        stub = self.stub_ccache('echo "ccache version 4.14.1"; echo "Features: file-storage http-storage"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.14.1 ({stub})")

    def test_minimum_version(self):
        stub = self.stub_ccache('echo "ccache version 4.9.1"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.9.1 ({stub})")

    def test_older_version_warns(self):
        stub = self.stub_ccache('echo "ccache version 4.2"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.2 ({stub}) - 4.9.1+ recommended")

    def test_unparseable_version(self):
        stub = self.stub_ccache('echo "ccache, but not a version line"')
        self.assertEqual(
            self.ccache_row(self.run_checker()), f"ccache: version not recognized ({stub}) - 4.9.1+ recommended"
        )

    def test_failing_ccache(self):
        stub = self.stub_ccache('echo "broken" >&2; exit 3')
        self.assertEqual(
            self.ccache_row(self.run_checker()), f"ccache: version not recognized ({stub}) - 4.9.1+ recommended"
        )

    def test_absent_ccache(self):
        out = self.run_checker()
        self.assertEqual(self.ccache_row(out), "ccache: not found (recommended for faster rebuilds)")


if __name__ == "__main__":
    unittest.main()
