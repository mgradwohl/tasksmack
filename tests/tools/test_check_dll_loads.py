#!/usr/bin/env python3
"""Tests for tools/check-dll-loads.py (#2530): every DLL that src/ loads must be loaded with a restricted
search path, so a same-named DLL next to a portable TaskSmack.exe is never picked up."""

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check-dll-loads.py"
spec = importlib.util.spec_from_file_location("check_dll_loads", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class CheckTextTest(unittest.TestCase):
    def test_bare_loads_fail(self) -> None:
        for call in ("LoadLibrary", "LoadLibraryA", "LoadLibraryW"):
            with self.subTest(call=call):
                errors = checker.check_text(f'void f()\n{{\n    m.reset({call}(L"pdh.dll"));\n}}\n', "a.cpp")
                self.assertEqual(len(errors), 1, errors)
                self.assertTrue(errors[0].startswith("a.cpp:3: "), errors[0])

    def test_ex_without_search_flag_fails(self) -> None:
        errors = checker.check_text('HMODULE m = LoadLibraryExW(L"pdh.dll", nullptr, 0);\n', "a.cpp")
        self.assertEqual(len(errors), 1, errors)

    def test_ex_with_system32_passes(self) -> None:
        text = (
            'HMODULE m = LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);\n'
            "auto load = [] -> void* {\n"
            '    return LoadLibraryExW(L"nvml.dll",\n'
            "                          nullptr,\n"
            "                          LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
            "};\n"
        )
        self.assertEqual(checker.check_text(text, "a.cpp"), [])

    def test_comments_and_other_names_are_ignored(self) -> None:
        text = (
            "// would otherwise call LoadLibraryW(L\"x.dll\") again\n"
            "/* LoadLibraryA(\"y.dll\") */\n"
            "inline void* fakeLoadLibrary() { return nullptr; }\n"
            'const char* url = "http://example.com"; // LoadLibraryW(\n'
        )
        self.assertEqual(checker.check_text(text, "a.cpp"), [])

    def test_line_numbers_survive_block_comments(self) -> None:
        text = '/* one\n two\n*/\nLoadLibraryW(L"a.dll");\n'
        errors = checker.check_text(text, "a.cpp")
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith("a.cpp:4: "), errors[0])


class CheckTreeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="check-dll-loads-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def test_scans_sources_under_src(self) -> None:
        (self.root / "src" / "Platform" / "Windows").mkdir(parents=True)
        (self.root / "src" / "Platform" / "Windows" / "Probe.h").write_text('LoadLibraryW(L"pdh.dll");\n', encoding="utf-8")
        (self.root / "src" / "notes.txt").write_text('LoadLibraryW(L"pdh.dll");\n', encoding="utf-8")
        errors = checker.check(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith("src/Platform/Windows/Probe.h:1: "), errors[0])

    def test_missing_src_fails(self) -> None:
        self.assertEqual(len(checker.check(self.root)), 1)

    def test_repository_sources_pass(self) -> None:
        self.assertEqual(checker.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
