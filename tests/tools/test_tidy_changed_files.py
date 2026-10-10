#!/usr/bin/env python3
"""Tests for tools/tidy-changed-files.py: which translation units a PR's clang-tidy job checks (#1406),
in particular that a CMakeLists.txt edit that only adds/removes source-list entries no longer forces
a whole-repo run (#1626)."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "tidy-changed-files.py"

ROOT_CMAKE = """\
cmake_minimum_required(VERSION 3.28)
project(Mini CXX)

set(MINI_VERSION 1)
add_compile_options(-Wall)

set(TASKSMACK_SOURCES
    src/main.cpp
    src/Core/A.cpp
    src/Core/B.cpp
)

# Platform-specific sources
set(PLATFORM_SOURCES_LINUX
    src/Platform/Linux/L.cpp
)

set(PLATFORM_SOURCES_WINDOWS
    src/Platform/Windows/W.cpp
)

set(TASKSMACK_HEADERS
    src/Core/A.h
)

if(WIN32)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Windows/W.h
    )
endif()

add_library(App OBJECT ${TASKSMACK_SOURCES})
"""

FILES = {
    "src/main.cpp": "int main() { return 0; }\n",
    "src/Core/A.h": "#pragma once\nint a();\n",
    "src/Core/A.cpp": '#include "A.h"\nint a() { return 1; }\n',
    "src/Core/B.cpp": "int b() { return 2; }\n",
    "src/Core/Orphan.cpp": "int orphan() { return 3; }\n",  # on disk, listed nowhere yet
    "src/Platform/Linux/L.cpp": "int l() { return 4; }\n",
    "src/Platform/Windows/W.h": "#pragma once\n",
    "src/Platform/Windows/W.cpp": "int w() { return 5; }\n",
}


class TidyChangedFilesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Test")
        self.git("config", "core.autocrlf", "false")
        self.write("CMakeLists.txt", ROOT_CMAKE)
        for path, text in FILES.items():
            self.write(path, text)
        self.commit("base")
        self.base = self.git("rev-parse", "HEAD").strip()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", *args], cwd=self.repo, check=True, capture_output=True, text=True).stdout

    def write(self, path: str, text: str) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode("utf-8"))

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def edit_cmake(self, old: str, new: str) -> None:
        text = (self.repo / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(old, text)
        self.write("CMakeLists.txt", text.replace(old, new, 1))

    def run_script(self, platform: str = "linux") -> list[str]:
        result = subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--base", self.base, "--head", "HEAD", "--platform", platform],
            cwd=self.repo,
            capture_output=True,
            env={**os.environ, "PYTHONUTF8": "1"},
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"\r", result.stdout, "output must stay LF-only (ci.yml compares the first line)")
        return result.stdout.decode().splitlines()

    # -- list-only edits: no full run -------------------------------------------------------------

    def test_listing_a_new_cpp_selects_it(self) -> None:
        self.write("src/Core/New.cpp", "int n() { return 6; }\n")
        self.edit_cmake("    src/Core/B.cpp\n", "    src/Core/B.cpp\n    src/Core/New.cpp\n")
        self.commit("add New.cpp")
        self.assertEqual(self.run_script(), ["src/Core/New.cpp"])

    def test_listing_an_existing_cpp_selects_it(self) -> None:
        # Only CMakeLists.txt changes: the file itself is not in the diff, the list entry selects it.
        self.edit_cmake("    src/Core/B.cpp\n", "    src/Core/B.cpp\n    src/Core/Orphan.cpp\n")
        self.commit("list Orphan.cpp")
        self.assertEqual(self.run_script(), ["src/Core/Orphan.cpp"])

    def test_listing_a_new_header_is_not_a_full_run(self) -> None:
        self.write("src/Core/New.h", "#pragma once\n")
        self.edit_cmake("    src/Core/A.h\n", "    src/Core/A.h\n    src/Core/New.h\n")
        self.commit("add New.h")
        self.assertEqual(self.run_script(), [])

    def test_listing_a_header_selects_its_includers(self) -> None:
        self.write("src/Core/New.h", "#pragma once\n")
        self.write("src/Core/B.cpp", '#include "New.h"\nint b() { return 2; }\n')
        self.edit_cmake("    src/Core/A.h\n", "    src/Core/A.h\n    src/Core/New.h\n")
        self.commit("add New.h, include it from B.cpp")
        self.assertEqual(self.run_script(), ["src/Core/B.cpp"])

    def test_windows_only_cpp_is_selected_only_for_windows(self) -> None:
        self.write("src/Platform/Windows/W2.cpp", "int w2() { return 7; }\n")
        self.edit_cmake("    src/Platform/Windows/W.cpp\n", "    src/Platform/Windows/W.cpp\n    src/Platform/Windows/W2.cpp\n")
        self.commit("add W2.cpp")
        self.assertEqual(self.run_script("linux"), [])
        self.assertEqual(self.run_script("windows"), ["src/Platform/Windows/W2.cpp"])

    def test_list_append_quoted_and_prefixed_entries_and_comments(self) -> None:
        self.write("src/Platform/Windows/W2.h", "#pragma once\n")
        self.write("src/Core/C.cpp", "int c() { return 8; }\n")
        self.write("src/Core/D.cpp", "int d() { return 9; }\n")
        self.edit_cmake("        src/Platform/Windows/W.h\n", "        src/Platform/Windows/W.h\n        src/Platform/Windows/W2.h\n")
        self.edit_cmake(
            "    src/Core/B.cpp\n",
            '    src/Core/B.cpp\n\n    # New files\n    "src/Core/C.cpp"\n    ${CMAKE_CURRENT_SOURCE_DIR}/src/Core/D.cpp  # D\n',
        )
        self.commit("add several")
        self.assertEqual(self.run_script(), ["src/Core/C.cpp", "src/Core/D.cpp"])

    def test_removing_an_entry_is_not_a_full_run(self) -> None:
        (self.repo / "src/Core/B.cpp").unlink()
        self.edit_cmake("    src/Core/B.cpp\n", "")
        self.commit("remove B.cpp")
        self.assertEqual(self.run_script(), [])

    # -- anything else in a CMakeLists.txt: full run ----------------------------------------------

    def test_compile_option_change_is_a_full_run(self) -> None:
        self.edit_cmake("add_compile_options(-Wall)\n", "add_compile_options(-Wall -Wextra)\n")
        self.commit("flags")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_mixed_change_is_a_full_run(self) -> None:
        self.write("src/Core/New.cpp", "int n() { return 6; }\n")
        self.edit_cmake("    src/Core/B.cpp\n", "    src/Core/B.cpp\n    src/Core/New.cpp\n")
        self.edit_cmake("set(MINI_VERSION 1)\n", "set(MINI_VERSION 2)\n")
        self.commit("add New.cpp and bump a variable")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_entry_outside_a_source_list_is_a_full_run(self) -> None:
        self.write("src/Core/New.cpp", "int n() { return 6; }\n")
        self.edit_cmake("add_library(App OBJECT ${TASKSMACK_SOURCES})\n", "add_library(App OBJECT ${TASKSMACK_SOURCES}\n    src/Core/New.cpp\n)\n")
        self.commit("add New.cpp to a target")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_entry_after_a_closed_list_is_a_full_run(self) -> None:
        self.edit_cmake("    src/Core/A.h\n)\n", "    src/Core/A.h\n)\nsrc/Core/Orphan.cpp\n")
        self.commit("stray entry")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_bracket_comment_is_a_full_run(self) -> None:
        self.edit_cmake("add_compile_options(-Wall)\n", "#[[\nadd_compile_options(-Wall)\n]]\n")
        self.commit("comment out flags")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_new_src_cmakelists_is_a_full_run(self) -> None:
        self.write("src/Core/CMakeLists.txt", "set(CORE_SOURCES\n    A.cpp\n)\n")
        self.commit("sub-CMakeLists")
        self.assertEqual(self.run_script(), ["ALL"])

    def test_tests_cmakelists_is_ignored(self) -> None:
        self.write("tests/CMakeLists.txt", "add_compile_options(-Werror)\n")
        self.commit("tests only")
        self.assertEqual(self.run_script(), [])


if __name__ == "__main__":
    unittest.main()
