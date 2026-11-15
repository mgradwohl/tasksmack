#!/usr/bin/env python3
"""Tests for tools/check-dll-loads.py (#2530): every DLL that src/ loads must be loaded with a restricted
search path, so a same-named DLL next to a portable TaskSmack.exe is never picked up; and each entry point that
can run elevated restricts the default DLL directories first (#3897)."""

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


def entry_point(function: str, body: str) -> str:
    """A definition of @p function whose body is @p body."""
    return f"int WINAPI {function}(HINSTANCE /*instance*/, HINSTANCE, LPSTR, int)\n{{\n{body}}}\n"


class CheckEntryPointTest(unittest.TestCase):
    def test_first_call_passes(self) -> None:
        body = (
            "    // First, before anything loads a DLL.\n"
            "    SetDefaultDllDirectories(elevated() ? LOAD_LIBRARY_SEARCH_SYSTEM32\n"
            "                                        : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
            "    return run();\n"
        )
        self.assertEqual(checker.check_entry_point(entry_point("WinMain", body), "main.cpp", "WinMain"), [])

    def test_trailing_return_type_passes(self) -> None:
        text = "auto WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) -> int\n{\n    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n}\n"
        self.assertEqual(checker.check_entry_point(text, "main.cpp", "WinMain"), [])

    def test_namespace_aliases_may_come_first(self) -> None:
        body = "    namespace Helper = Platform::Windows::BrokerHelper;\n    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
        self.assertEqual(checker.check_entry_point(entry_point("wWinMain", body), "HelperMain.cpp", "wWinMain"), [])

    def test_a_later_call_fails(self) -> None:
        body = "    SDL_Init(SDL_INIT_VIDEO);\n    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
        errors = checker.check_entry_point("\n" + entry_point("WinMain", body), "main.cpp", "WinMain")
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith("main.cpp:2: "), errors[0])

    def test_a_missing_or_commented_out_call_fails(self) -> None:
        for body in ("    return run();\n", "    // SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n    return run();\n"):
            with self.subTest(body=body):
                self.assertEqual(len(checker.check_entry_point(entry_point("WinMain", body), "main.cpp", "WinMain")), 1)

    def test_wide_or_missing_system32_flags_fail(self) -> None:
        for flags in (
            "LOAD_LIBRARY_SEARCH_DEFAULT_DIRS",
            "LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS",
            "LOAD_LIBRARY_SEARCH_APPLICATION_DIR",
        ):
            with self.subTest(flags=flags):
                text = entry_point("WinMain", f"    SetDefaultDllDirectories({flags});\n")
                self.assertEqual(len(checker.check_entry_point(text, "main.cpp", "WinMain")), 1)

    def test_only_a_declaration_or_another_name_fails(self) -> None:
        for text in (
            "int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);\n",
            entry_point("wWinMain", "    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n"),
        ):
            with self.subTest(text=text):
                self.assertEqual(len(checker.check_entry_point(text, "main.cpp", "WinMain")), 1)


class CheckTreeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="check-dll-loads-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write_entry_points(self) -> None:
        for relative, function in checker.ENTRY_POINTS:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            body = "    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
            path.write_text(entry_point(function, body), encoding="utf-8")

    def test_scans_sources_under_src(self) -> None:
        self.write_entry_points()
        (self.root / "src" / "Platform" / "Windows" / "Probe.h").write_text('LoadLibraryW(L"pdh.dll");\n', encoding="utf-8")
        (self.root / "src" / "notes.txt").write_text('LoadLibraryW(L"pdh.dll");\n', encoding="utf-8")
        errors = checker.check(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith("src/Platform/Windows/Probe.h:1: "), errors[0])

    def test_missing_src_fails(self) -> None:
        self.assertEqual(checker.check(self.root), ["src: missing"])

    def test_missing_entry_point_fails(self) -> None:
        self.write_entry_points()
        missing = checker.ENTRY_POINTS[0][0]
        (self.root / missing).unlink()
        self.assertEqual(checker.check(self.root), [f"{missing.as_posix()}: missing"])

    def test_repository_sources_pass(self) -> None:
        self.assertEqual(checker.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
