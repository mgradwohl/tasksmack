#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 TaskSmack Contributors
# SPDX-License-Identifier: MIT
"""The Clang floor is one number (#4352): cmake/CompilerOptions.cmake's TASKSMACK_MIN_CLANG_VERSION, which stops a
configure with an older or another compiler, is the MIN_CLANG_VERSION tools/check-prereqs.ps1 and .sh check, so the
error never names a floor check-prereqs disagrees with. The error names the platform's own setup and check scripts."""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
COMPILER_OPTIONS = REPO / "cmake" / "CompilerOptions.cmake"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ClangFloorTest(unittest.TestCase):
    def floor(self, path: Path, pattern: str) -> int:
        match = re.search(pattern, read(path), re.MULTILINE)
        self.assertIsNotNone(match, f"no Clang floor in {path.relative_to(REPO)}")
        assert match is not None
        return int(match.group(1))

    def test_the_three_floors_are_one_number(self) -> None:
        cmake = self.floor(COMPILER_OPTIONS, r"^set\(TASKSMACK_MIN_CLANG_VERSION (\d+)\)$")
        ps1 = self.floor(REPO / "tools" / "check-prereqs.ps1", r"^\$MIN_CLANG_VERSION = (\d+)$")
        sh = self.floor(REPO / "tools" / "check-prereqs.sh", r'^MIN_CLANG_VERSION="(\d+)"$')
        self.assertEqual({cmake, ps1, sh}, {cmake}, f"CMake {cmake}, check-prereqs.ps1 {ps1}, check-prereqs.sh {sh}")

    def test_the_error_names_each_platforms_scripts(self) -> None:
        text = read(COMPILER_OPTIONS)
        for command in (
            "pwsh tools/setup-dev.ps1",
            "pwsh tools/check-prereqs.ps1",
            "./tools/setup-dev.sh",
            "./tools/check-prereqs.sh",
        ):
            self.assertIn(f"`{command}`", text)
            script = REPO / command.split()[-1]
            self.assertTrue(script.is_file(), f"the error names {command}, which doesn't exist")

    def test_any_other_compiler_and_an_older_clang_stop_the_configure(self) -> None:
        text = read(COMPILER_OPTIONS)
        self.assertRegex(text, r'if\(NOT CMAKE_\$\{_ts_lang\}_COMPILER_ID STREQUAL "Clang"\)\s*\n\s*message\(FATAL_ERROR')
        self.assertRegex(text, r"VERSION_LESS TASKSMACK_MIN_CLANG_VERSION\)\s*\n\s*message\(FATAL_ERROR")
        self.assertNotRegex(text, r'COMPILER_ID MATCHES "GNU"|elseif\(MSVC\)', "no branches for compilers it refuses")
        # Before anything else in the file uses the compiler.
        self.assertLess(text.index("set(TASKSMACK_MIN_CLANG_VERSION"), text.index("CMAKE_MSVC_RUNTIME_LIBRARY"))


if __name__ == "__main__":
    unittest.main()
