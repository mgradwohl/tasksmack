#!/usr/bin/env python3
"""tools/pgo.sh's help and an unknown action (#4216): help asked for exits 0, as the other scripts' -h does
(#2276); an action it doesn't know prints the usage and exits 1. Neither builds anything. Linux only."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
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


if __name__ == "__main__":
    unittest.main()
