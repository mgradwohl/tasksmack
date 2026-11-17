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


POLICY = "Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()"
MAIN_CALL = (
    f"    SetDefaultDllDirectories({POLICY}\n"
    "                                 ? LOAD_LIBRARY_SEARCH_SYSTEM32\n"
    "                                 : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);\n"
)
# What TaskSmack.exe's WinMain must call next, before any SDL call (#4103)
SCRUB_ARGUMENT = POLICY
SDL_SCRUB = f"    Platform::Windows::SdlEnvironment::removeForThisProcess({SCRUB_ARGUMENT});\n"
# And the check that fails closed (#4114, #4169)
FAIL_CLOSED = (
    "    if (const auto& stillSet = Platform::Windows::SdlEnvironment::stillSetInThisProcess(); !stillSet.empty())\n"
    "    {\n"
    '        MessageBoxW(nullptr, L"refused {", L"TaskSmack", MB_OK);\n'
    "        return EXIT_FAILURE;\n"
    "    }\n"
)
MAIN_START = MAIN_CALL + SDL_SCRUB + FAIL_CLOSED
HELPER_CALL = "    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n"


def entry_point(function: str, body: str) -> str:
    """A definition of @p function whose body is @p body."""
    return f"int WINAPI {function}(HINSTANCE /*instance*/, HINSTANCE, LPSTR, int)\n{{\n{body}}}\n"


def check_main(text: str) -> list[str]:
    return checker.check_entry_point(text, "main.cpp", "WinMain", checker.BRANCH)


def check_helper(text: str) -> list[str]:
    return checker.check_entry_point(text, "HelperMain.cpp", "wWinMain", checker.EXACT)


class CheckEntryPointTest(unittest.TestCase):
    def test_main_with_its_system32_branch_first_passes(self) -> None:
        body = "    // First, before anything loads a DLL.\n" + MAIN_START + "    return run();\n"
        self.assertEqual(check_main(entry_point("WinMain", body)), [])

    def test_trailing_return_type_passes(self) -> None:
        text = "auto WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) -> int\n{\n" + MAIN_START + "}\n"
        self.assertEqual(check_main(text), [])

    def test_helper_with_exactly_system32_after_an_alias_passes(self) -> None:
        body = "    namespace Helper = Platform::Windows::BrokerHelper;\n" + HELPER_CALL
        self.assertEqual(check_helper(entry_point("wWinMain", body)), [])

    def test_a_later_call_fails(self) -> None:
        body = "    SDL_Init(SDL_INIT_VIDEO);\n" + MAIN_CALL
        errors = check_main("\n" + entry_point("WinMain", body))
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith("main.cpp:2: "), errors[0])

    def test_a_missing_or_commented_out_call_fails(self) -> None:
        for body in ("    return run();\n", "    // SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);\n    return run();\n"):
            with self.subTest(body=body):
                self.assertEqual(len(check_main(entry_point("WinMain", body))), 1)
                self.assertEqual(len(check_helper(entry_point("wWinMain", body))), 1)

    def test_the_helper_must_pass_exactly_system32(self) -> None:
        for flags in (
            "LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32",
            "LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR",
            "LOAD_LIBRARY_SEARCH_APPLICATION_DIR",
            "elevated() ? LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32",
            "LOAD_LIBRARY_SEARCH_DEFAULT_DIRS",
        ):
            with self.subTest(flags=flags):
                self.assertEqual(len(check_helper(entry_point("wWinMain", f"    SetDefaultDllDirectories({flags});\n"))), 1)

    def test_main_must_keep_its_system32_only_branch(self) -> None:
        for flags in (
            "LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32",
            "LOAD_LIBRARY_SEARCH_SYSTEM32",
            "elevated() ? LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR : LOAD_LIBRARY_SEARCH_SYSTEM32",
            "elevated() ? LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_APPLICATION_DIR",
        ):
            with self.subTest(flags=flags):
                self.assertEqual(len(check_main(entry_point("WinMain", f"    SetDefaultDllDirectories({flags});\n"))), 1)

    def test_wide_flags_fail_in_either_branch(self) -> None:
        for flags in (
            "elevated() ? LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS",
            "elevated() ? LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS",
        ):
            with self.subTest(flags=flags):
                self.assertEqual(len(check_main(entry_point("WinMain", f"    SetDefaultDllDirectories({flags});\n"))), 1)

    def test_main_must_remove_the_sdl_variables_next(self) -> None:
        # #4103: right after SetDefaultDllDirectories, before anything can make an SDL call.
        for body in (
            MAIN_CALL + "    return run();\n",
            MAIN_CALL + "    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, \"windows\");\n" + SDL_SCRUB,
            MAIN_CALL + "    // " + SDL_SCRUB.lstrip(),
            MAIN_CALL + "    SdlEnvironment::removedFromThisProcess();\n",
            MAIN_CALL + "    static_cast<void>(0), SdlEnvironment::removeForThisProcess(x);\n",
        ):
            with self.subTest(body=body):
                errors = check_main(entry_point("WinMain", body))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("SdlEnvironment::removeForThisProcess(", errors[0])
                self.assertIn("#4103", errors[0])

    def test_the_sdl_scrub_must_be_passed_the_system32_only_rule(self) -> None:
        # #4114: the call alone isn't enough; removeForThisProcess(false) removes nothing.
        for argument in (
            "false",
            "true",
            "x",
            "system32Only()",
            "!Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()",
            "false && Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()",
            "Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess() && false",
            "false, Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()",
            "notsystem32OnlyForThisProcess()",
            '"system32OnlyForThisProcess()"',
        ):
            with self.subTest(argument=argument):
                scrub = f"    Platform::Windows::SdlEnvironment::removeForThisProcess({argument});\n"
                errors = check_main(entry_point("WinMain", MAIN_CALL + scrub))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4114", errors[0])
                self.assertIn(checker.SCRUB_ARGUMENT, errors[0])

    def test_the_sdl_scrub_argument_may_span_lines(self) -> None:
        scrub = f"    Platform::Windows::SdlEnvironment::removeForThisProcess(\n        {SCRUB_ARGUMENT});\n"
        self.assertEqual(check_main(entry_point("WinMain", MAIN_CALL + scrub + FAIL_CLOSED)), [])

    def test_set_default_dll_directories_must_be_a_statement_of_its_own(self) -> None:
        # #4114: extra code in the same statement, before its ';', is code the scrub's position check never sees.
        call = MAIN_CALL.rstrip().removesuffix(";")
        for extra in (', SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "windows")', " + 0", " , run()"):
            with self.subTest(extra=extra):
                errors = check_main(entry_point("WinMain", call + extra + ";\n" + SDL_SCRUB + FAIL_CLOSED))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4114", errors[0])
                self.assertIn("statement of its own", errors[0])

    def test_whitespace_before_the_semicolon_is_fine(self) -> None:
        call = MAIN_CALL.rstrip().removesuffix(";")
        self.assertEqual(check_main(entry_point("WinMain", call + " \n    ;\n" + SDL_SCRUB + FAIL_CLOSED)), [])

    def test_the_sdl_scrub_may_be_qualified_or_not_after_comments(self) -> None:
        for scrub in (
            "    SdlEnvironment::removeForThisProcess(DllSearchPolicy::system32OnlyForThisProcess());\n",
            "    // Next, before any SDL call.\n    /* why */ Platform::Windows::SdlEnvironment::removeForThisProcess(::Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess());\n",
            "    Platform::Windows::SdlEnvironment::removeForThisProcess(::DllSearchPolicy::system32OnlyForThisProcess());\n",
        ):
            with self.subTest(scrub=scrub):
                self.assertEqual(check_main(entry_point("WinMain", MAIN_CALL + scrub + FAIL_CLOSED)), [])

    def test_the_policy_qualifier_is_pinned_to_dll_search_policy(self) -> None:
        # #4169: any other qualifier, or none, names some other function that may answer differently.
        for qualifier in (
            "",
            "Other::",
            "Platform::Other::",
            "Windows::Other::",
            "Platform::DllSearchPolicy::Nested::",
            "Platform::Windows::Other::DllSearchPolicy::",
        ):
            call = f"{qualifier}system32OnlyForThisProcess()"
            scrub = f"    Platform::Windows::SdlEnvironment::removeForThisProcess({call});\n"
            with self.subTest(scrub_argument=call):
                errors = check_main(entry_point("WinMain", MAIN_CALL + scrub + FAIL_CLOSED))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4114", errors[0])
            with self.subTest(set_default_argument=call):
                errors = check_main(entry_point("WinMain", MAIN_CALL.replace(POLICY, call) + SDL_SCRUB + FAIL_CLOSED))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4169", errors[0])

    def test_set_default_arguments_accept_the_global_qualifier_and_any_spacing(self) -> None:
        for policy in (
            "::Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()",
            "DllSearchPolicy::system32OnlyForThisProcess( )",
            "Platform :: Windows :: DllSearchPolicy :: system32OnlyForThisProcess ()",
        ):
            with self.subTest(policy=policy):
                call = MAIN_CALL.replace(POLICY, policy)
                self.assertEqual(check_main(entry_point("WinMain", call + SDL_SCRUB + FAIL_CLOSED)), [])

    def test_set_default_arguments_must_hold_no_sdl_identifier_and_no_other_call(self) -> None:
        # #4169: code in the argument runs before the scrub, so an SDL call there reads the variables the scrub removes.
        for extra in ("SDL_GetError()", "SDL_INIT_VIDEO", "SDL3_DYNAMIC_API", "helper()", "static_cast<DWORD>(0)"):
            with self.subTest(extra=extra):
                call = MAIN_CALL.replace("LOAD_LIBRARY_SEARCH_APPLICATION_DIR |", f"{extra} | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |")
                errors = check_main(entry_point("WinMain", call + SDL_SCRUB + FAIL_CLOSED))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4169", errors[0])
                self.assertIn("no SDL_ identifier", errors[0])

    def test_set_default_arguments_must_match_the_one_expected_shape(self) -> None:
        sys32 = "LOAD_LIBRARY_SEARCH_SYSTEM32"
        app = "LOAD_LIBRARY_SEARCH_APPLICATION_DIR"
        for flags in (
            f"{POLICY} ? {sys32} : {sys32} | {app}",
            f"{POLICY} ? {sys32} : {app} | {sys32} | 0",
            f"!{POLICY} ? {sys32} : {app} | {sys32}",
            f"true && {POLICY} ? {sys32} : {app} | {sys32}",
            f"{POLICY} ? {sys32} : {sys32}",
        ):
            with self.subTest(flags=flags):
                body = f"    SetDefaultDllDirectories({flags});\n" + SDL_SCRUB + FAIL_CLOSED
                errors = check_main(entry_point("WinMain", body))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4169", errors[0])

    def test_the_scrub_must_be_followed_by_the_fail_closed_check(self) -> None:
        # #4169: without it a scrub that couldn't clear a variable lets TaskSmack run on to SDL.
        scrub = MAIN_CALL + SDL_SCRUB
        for name, body in (
            ("nothing", scrub + "    return run();\n"),
            ("another statement first", scrub + "    log();\n" + FAIL_CLOSED),
            ("not stillSetInThisProcess", scrub + FAIL_CLOSED.replace("stillSetInThisProcess", "removedFromThisProcess")),
            ("not negated", scrub + FAIL_CLOSED.replace("!stillSet.empty()", "stillSet.empty()")),
            ("no braces", scrub + FAIL_CLOSED.replace("    {\n", "").replace("    }\n", "")),
            ("no return", scrub + FAIL_CLOSED.replace("return EXIT_FAILURE;", "log();")),
            ("a return after the block", scrub + FAIL_CLOSED.replace("        return EXIT_FAILURE;\n", "") + "    return EXIT_FAILURE;\n"),
            ("a comment only", scrub + "    // if (!stillSetInThisProcess().empty()) { return 1; }\n    return run();\n"),
        ):
            with self.subTest(name=name):
                errors = check_main(entry_point("WinMain", body))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("#4169", errors[0])

    def test_the_scrub_must_be_a_statement_of_its_own(self) -> None:
        scrub = SDL_SCRUB.rstrip().removesuffix(";") + ", log();\n"
        errors = check_main(entry_point("WinMain", MAIN_CALL + scrub + FAIL_CLOSED))
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("statement of its own", errors[0])

    def test_a_second_scrub_call_fails(self) -> None:
        # #4169: removeForThisProcess() clears the names still set, so a second call before the check would hide them.
        for body in (
            MAIN_CALL + SDL_SCRUB + FAIL_CLOSED + SDL_SCRUB + "    return run();\n",
            MAIN_CALL + SDL_SCRUB + FAIL_CLOSED + "    helper();\n    return run();\n" + SDL_SCRUB,
        ):
            with self.subTest(body=body):
                errors = check_main(entry_point("WinMain", body))
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("exactly once", errors[0])

    def test_a_brace_in_a_string_does_not_end_the_check_early(self) -> None:
        # FAIL_CLOSED's message holds a "{"; the body that follows is still the function's.
        self.assertEqual(check_main(entry_point("WinMain", MAIN_START + "    return run();\n")), [])
        self.assertEqual(checker.matching_brace('{ "}" }', 0), 6)

    def test_the_helper_needs_no_sdl_scrub(self) -> None:
        # TaskSmackHelper.exe makes no SDL call.
        self.assertEqual(check_helper(entry_point("wWinMain", HELPER_CALL + "    return run();\n")), [])

    def test_only_a_declaration_or_another_name_fails(self) -> None:
        for text in ("int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);\n", entry_point("wWinMain", MAIN_CALL)):
            with self.subTest(text=text):
                self.assertEqual(len(check_main(text)), 1)


class CheckTreeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="check-dll-loads-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write_entry_points(self) -> None:
        for relative, function, rule in checker.ENTRY_POINTS:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(entry_point(function, MAIN_START if rule == checker.BRANCH else HELPER_CALL), encoding="utf-8")

    def test_each_entry_point_has_its_own_rule(self) -> None:
        # The helper's own folder is never searched; the app keeps the branch it takes elevated.
        rules = {relative.as_posix(): rule for relative, _, rule in checker.ENTRY_POINTS}
        self.assertEqual(rules, {"src/main.cpp": checker.BRANCH, "src/Platform/Windows/HelperMain.cpp": checker.EXACT})

    def test_the_repositorys_entry_points_fail_once_mutated(self) -> None:
        # The real files, each with the change the rules exist to refuse.
        mutations = {
            "src/Platform/Windows/HelperMain.cpp": (
                "SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);",
                "SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);",
            ),
            "src/main.cpp": ("? LOAD_LIBRARY_SEARCH_SYSTEM32", "? LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32"),
        }
        for relative, function, rule in checker.ENTRY_POINTS:
            original = (REPO / relative).read_text(encoding="utf-8")
            self.assertEqual(checker.check_entry_point(original, relative.as_posix(), function, rule), [])
            old, new = mutations[relative.as_posix()]
            with self.subTest(file=relative.as_posix()):
                self.assertEqual(original.count(old), 1, old)
                mutated = original.replace(old, new)
                self.assertEqual(len(checker.check_entry_point(mutated, relative.as_posix(), function, rule)), 1)

    def test_the_repositorys_main_fails_without_its_sdl_scrub(self) -> None:
        relative = Path("src/main.cpp")
        original = (REPO / relative).read_text(encoding="utf-8")
        # The Windows scrub by its full name: the Linux main() makes its own call, Platform::SdlEnvironment's (#4111).
        old = "Windows::SdlEnvironment::removeForThisProcess("
        self.assertEqual(original.count(old), 1, old)
        mutated = original.replace(old, "Windows::SdlEnvironment::removedFromThisProcess(")
        self.assertEqual(len(checker.check_entry_point(mutated, relative.as_posix(), "WinMain", checker.BRANCH)), 1)

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
