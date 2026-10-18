#!/usr/bin/env python3
"""Tests for tools/setup-dev.sh's virtual environment guard (#2351).

A checkout shared between Windows and WSL can hold the Windows venv setup-dev.ps1 creates. Linux
setup must stop before `python -m venv` rewrites that venv's pyvenv.cfg (warn only under
--dry-run), and still create a missing venv or reuse a Linux one. The script is sourced, so nothing
is installed, and python3.14 is a shell function that records its arguments. Needs bash; registered
in CTest only where bash is available.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SETUP_SH = REPO_ROOT / "tools" / "setup-dev.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

WINDOWS_CFG = "home = C:\\Python314\nversion = 3.14.7\nexecutable = C:\\Python314\\python.exe\n"

# Stands in for python3.14: logs its arguments, and for `-m venv DIR` writes a Linux venv as the
# real one would, pyvenv.cfg included.
STUB = r"""
python3.14() {
    printf '%s\n' "$*" >>"$STUB_LOG"
    if [[ "$1" == "-m" && "$2" == "venv" ]]; then
        mkdir -p "$3/bin"
        : >"$3/bin/python"
        printf 'home = /usr/bin\n' >"$3/pyvenv.cfg"
    fi
}
"""


def run_create_venv(venv, log, *args):
    """Sources setup-dev.sh with *args, then runs create_venv on venv."""
    script = f'p=$1; shift; source "$p" "$@"; {STUB} create_venv "$VENV_DIR"'
    env = dict(os.environ, VENV_DIR=venv.as_posix(), STUB_LOG=log.as_posix())
    return subprocess.run(
        [BASH, "-c", script, "test", SETUP_SH.as_posix(), *args],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )


class SetupDevVenvTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.venv = self.root / ".venv"
        self.log = self.root / "python.log"

    def tearDown(self):
        self._tmp.cleanup()

    def make_windows_venv(self):
        (self.venv / "Scripts").mkdir(parents=True)
        (self.venv / "Scripts" / "python.exe").write_bytes(b"MZ")
        (self.venv / "pyvenv.cfg").write_text(WINDOWS_CFG, encoding="utf-8", newline="")

    def python_calls(self):
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def test_windows_venv_stops_untouched(self):
        self.make_windows_venv()
        result = run_create_venv(self.venv, self.log)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("has no bin/python (a Windows venv from setup-dev.ps1?)", result.stderr)
        self.assertIn("Remove or rename it, then rerun this script.", result.stderr)
        self.assertEqual(self.python_calls(), [])
        self.assertEqual((self.venv / "pyvenv.cfg").read_text(encoding="utf-8", newline=""), WINDOWS_CFG)
        self.assertFalse((self.venv / "bin").exists())

    def test_windows_venv_dry_run_warns(self):
        self.make_windows_venv()
        result = run_create_venv(self.venv, self.log, "--dry-run")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARNING:", result.stderr)
        self.assertIn("a real run would stop here", result.stderr)
        self.assertIn("[dry-run] python3.14 -m venv", result.stdout)
        self.assertEqual(self.python_calls(), [])
        self.assertEqual((self.venv / "pyvenv.cfg").read_text(encoding="utf-8", newline=""), WINDOWS_CFG)

    def test_missing_venv_is_created(self):
        result = run_create_venv(self.venv, self.log)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.python_calls(), [f"-m venv {self.venv.as_posix()}"])
        self.assertTrue((self.venv / "bin" / "python").exists())
        self.assertNotIn("Using existing", result.stdout)

    def test_linux_venv_is_reused(self):
        (self.venv / "bin").mkdir(parents=True)
        (self.venv / "bin" / "python").write_bytes(b"")
        result = run_create_venv(self.venv, self.log)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Using existing virtual environment", result.stdout)
        self.assertEqual(self.python_calls(), [f"-m venv {self.venv.as_posix()}"])

    def test_mixed_venv_is_treated_as_linux(self):
        # Both interpreters: a Linux venv over a Windows one; setup-dev.sh keeps using bin/python
        self.make_windows_venv()
        (self.venv / "bin").mkdir()
        (self.venv / "bin" / "python").write_bytes(b"")
        result = run_create_venv(self.venv, self.log)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Using existing virtual environment", result.stdout)


if __name__ == "__main__":
    unittest.main()
