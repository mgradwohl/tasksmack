#!/usr/bin/env python3
"""Tests for the installer's text encoding and PATH handling (#4300), in cmake/CPackProjectConfig.cmake.in
and cmake/Packaging.cmake.

The NSIS installer is a Unicode target: CPACK_NSIS_DEFINES, which CPack's template puts into project.nsi,
starts with `Unicode true`, so makensis builds the installer with the W registry and file APIs and raises no
warning 7998 (ANSI target). It has no "Add to PATH" option: CPACK_NSIS_MODIFY_PATH is never set, so the
template writes nothing to PATH (a PATH entry outside the ANSI code page was rewritten with `?`).

makensis isn't on every machine, so the generated script is checked from its sources: the bracket argument of
CPACK_NSIS_DEFINES is exactly what CPack puts at the template's @CPACK_NSIS_DEFINES@. When a project.nsi from a
real CPack run is named in TASKSMACK_NSIS_PROJECT, it is checked too.
"""

import os
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
PACKAGING = REPO / "cmake" / "Packaging.cmake"


def nsis_defines() -> list[str]:
    """The lines of CPACK_NSIS_DEFINES's bracket argument."""
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(r"set\(CPACK_NSIS_DEFINES \[=\[\n(.*?)\]=\]\)", text, re.DOTALL)
    if match is None:
        raise AssertionError("no CPACK_NSIS_DEFINES in " + str(CONFIG))
    return match.group(1).splitlines()


def code_lines(path: Path) -> list[str]:
    """The file's lines without comments and blank lines."""
    lines = path.read_text(encoding="utf-8").splitlines()
    return [line for line in lines if line.strip() and not line.lstrip().startswith("#")]


class NsisUnicodeTest(unittest.TestCase):
    def test_defines_start_with_unicode_true(self) -> None:
        lines = [line.strip() for line in nsis_defines() if line.strip()]
        self.assertEqual(lines[0], "Unicode true")

    def test_unicode_true_is_set_once(self) -> None:
        self.assertEqual(sum(line.strip().lower().startswith("unicode ") for line in nsis_defines()), 1)

    def test_no_path_option(self) -> None:
        for path in (PACKAGING, CONFIG):
            for line in code_lines(path):
                self.assertNotIn("CPACK_NSIS_MODIFY_PATH", line, str(path))

    def test_no_other_path_writes(self) -> None:
        # Nothing of ours reads or writes the PATH variable in the installer's script.
        for line in code_lines(CONFIG):
            self.assertNotRegex(line, r"(?i)AddToPath|EnVar|WriteRegExpandStr", line)

    @unittest.skipUnless(os.environ.get("TASKSMACK_NSIS_PROJECT"), "no CPack project.nsi named")
    def test_generated_project_nsi(self) -> None:
        text = Path(os.environ["TASKSMACK_NSIS_PROJECT"]).read_text(encoding="utf-8-sig")
        self.assertRegex(text, r"(?m)^\s*Unicode true\s*$")
        self.assertIn('StrCmp "" "ON" 0 doNotAddToPath', text)


if __name__ == "__main__":
    unittest.main()
