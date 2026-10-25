#!/usr/bin/env python3
"""Tests for tools/install-local.sh's launcher Exec line (#2342).

The installer writes its binary path into a desktop entry. Any path must come out as one Desktop
Entry Exec argument that unescapes back to itself, and must not pass through sed replacement
syntax. The functions are sourced, so nothing is installed. Needs bash; registered in CTest only
where bash is available.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALL_SH = REPO_ROOT / "tools" / "install-local.sh"
DESKTOP_SRC = REPO_ROOT / "assets" / "linux" / "app.tasksmack.TaskSmack.desktop"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

PATHS = [
    "/opt/tasksmack/bin/TaskSmack",
    "/tmp/tasksmack audit/bin/TaskSmack",
    "/tmp/tasksmack&audit/bin/TaskSmack",
    "/tmp/a|b#c/bin/TaskSmack",
    "/tmp/it's \"quoted\"/bin/TaskSmack",
    "/tmp/back\\slash/bin/TaskSmack",
    "/tmp/100%/bin/TaskSmack",
    "/tmp/$HOME `id`/bin/TaskSmack",
]


def run_bash(script, *args):
    return subprocess.run(
        # $0 stays "test", so the installer sees itself sourced rather than run
        [BASH, "-c", f'source "$1"; shift; {script}', "test", str(INSTALL_SH), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def unescape_exec(value):
    """Reads one Exec argument back as the Desktop Entry specification defines it."""
    # The string type's escapes first
    out, i = [], 0
    while i < len(value):
        if value[i] == "\\" and i + 1 < len(value):
            out.append({"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}.get(value[i + 1], "\\" + value[i + 1]))
            i += 2
        else:
            out.append(value[i])
            i += 1
    value = "".join(out)
    # Then the field codes: %% is a literal %, and any other % is a field code (not expected here)
    assert "%" not in value.replace("%%", ""), f"stray field code in {value!r}"
    value = value.replace("%%", "%")
    # Then the quoting rule: one double-quoted argument with \" \` \$ \\ escaped
    assert value.startswith('"') and value.endswith('"') and len(value) >= 2, value
    body, out, i = value[1:-1], [], 0
    while i < len(body):
        if body[i] == "\\":
            assert i + 1 < len(body) and body[i + 1] in '"`$\\', f"bad escape in {body!r}"
            out.append(body[i + 1])
            i += 2
        else:
            assert body[i] not in '"`$', f"unescaped {body[i]!r} in {body!r}"
            out.append(body[i])
            i += 1
    return "".join(out)


class DesktopExecQuoteTest(unittest.TestCase):
    def test_every_path_round_trips(self):
        for path in PATHS:
            with self.subTest(path=path):
                result = run_bash('desktop_exec_quote "$1"', path)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(unescape_exec(result.stdout), path)

    def test_a_newline_is_refused(self):
        result = run_bash('desktop_exec_quote "$1"', "/tmp/a\nb/TaskSmack")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("newline", result.stderr)


class SetDesktopExecTest(unittest.TestCase):
    def test_only_the_exec_line_changes(self):
        original = DESKTOP_SRC.read_text(encoding="utf-8").splitlines()
        with tempfile.TemporaryDirectory() as tmp:
            for path in PATHS:
                with self.subTest(path=path):
                    entry = Path(tmp) / "app.desktop"
                    shutil.copyfile(DESKTOP_SRC, entry)
                    result = run_bash('set_desktop_exec "$1" "$2"', str(entry), path)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    lines = entry.read_text(encoding="utf-8").splitlines()
                    self.assertEqual(len(lines), len(original))
                    for before, after in zip(original, lines):
                        if before.startswith("Exec="):
                            self.assertEqual(unescape_exec(after[len("Exec=") :]), path)
                        else:
                            self.assertEqual(after, before)

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-validate is not installed")
    def test_the_entry_still_validates(self):
        with tempfile.TemporaryDirectory() as tmp:
            entry = Path(tmp) / "app.tasksmack.TaskSmack.desktop"
            shutil.copyfile(DESKTOP_SRC, entry)
            result = run_bash('set_desktop_exec "$1" "$2"', str(entry), "/tmp/tasksmack&audit 100%/bin/TaskSmack")
            self.assertEqual(result.returncode, 0, result.stderr)
            validate = subprocess.run(["desktop-file-validate", str(entry)], capture_output=True, text=True, check=False)
            self.assertNotIn("error", validate.stdout.lower(), validate.stdout)


if __name__ == "__main__":
    unittest.main()
