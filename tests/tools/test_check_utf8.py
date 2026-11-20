#!/usr/bin/env python3
"""Tests for tools/check-utf8.py (#1648, #2529).

The pre-commit hook must refuse text that is not UTF-8, and text that was UTF-8 read as Windows-1252 or
Latin-1 and saved again ("mojibake"), such as the battery card's em dash that showed as U+00E2 plus two
garbage glyphs (#2529). Every non-ASCII sample here is built from code points, so this file stays
clean under the checker it tests.
"""

import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check-utf8.py"
spec = importlib.util.spec_from_file_location("check_utf8", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

EM_DASH = chr(0x2014)
# The em dash's UTF-8 bytes E2 80 94 read as Windows-1252, and as Latin-1 (two C1 controls; #2529).
EM_DASH_AS_1252 = "".join(map(chr, (0xE2, 0x20AC, 0x201D)))
EM_DASH_AS_LATIN1 = chr(0xE2) + "\u0080\u0094"
# U+00E9 (e acute) encoded twice.
E_ACUTE_TWICE = "".join(map(chr, (0xC3, 0xA9)))
E_ACUTE = chr(0xE9)


class FindMojibakeTest(unittest.TestCase):
    def test_a_double_encoded_em_dash_is_found_with_what_it_meant(self) -> None:
        for garbled in (EM_DASH_AS_1252, EM_DASH_AS_LATIN1):
            with self.subTest(garbled=garbled.encode("unicode_escape")):
                self.assertEqual(checker.find_mojibake(f"x {garbled} y"), [(3, garbled, EM_DASH)])

    def test_a_double_encoded_latin_letter_is_found(self) -> None:
        self.assertEqual(checker.find_mojibake(f"caf{E_ACUTE_TWICE}"), [(4, E_ACUTE_TWICE, E_ACUTE)])

    def test_correct_non_ascii_text_is_not_flagged(self) -> None:
        i_diaeresis, degree, a_tilde, half, a_circumflex = map(chr, (0xEF, 0xB0, 0xC3, 0xBD, 0xE2))
        for text in (
            EM_DASH,
            f"caf{E_ACUTE}",
            f"na{i_diaeresis}ve",
            f"{degree}C",
            f"{a_tilde} alone",
            f"1{half} {a_circumflex}",
        ):
            with self.subTest(text=text.encode("unicode_escape")):
                self.assertEqual(checker.find_mojibake(text), [])


class CheckFileTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_checker(self, data: bytes, name: str = "sample.cpp") -> subprocess.CompletedProcess:
        path = self.tmp / name
        path.write_bytes(data)
        return subprocess.run(
            [sys.executable, "-I", "-X", "utf8", str(SCRIPT), str(path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout=60,
        )

    def test_a_file_with_a_double_encoded_line_fails_and_names_it(self) -> None:
        text = f'ok\nconstexpr const char* NO_CHARGE_TEXT = "{EM_DASH_AS_LATIN1}";\n'
        result = self.run_checker(text.encode("utf-8"))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("sample.cpp:2:", result.stdout)
        self.assertIn("double-encoded", result.stdout)
        self.assertIn("U+2014", result.stdout)

    def test_a_clean_utf8_file_passes(self) -> None:
        result = self.run_checker(f'constexpr const char* DASH = "{EM_DASH}"; // caf{E_ACUTE}\n'.encode("utf-8"))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, "")

    def test_invalid_utf8_and_a_utf16_bom_fail(self) -> None:
        for data in (b"ok\n\xff\xfe bad\n", "x\n".encode("utf-16")):
            with self.subTest(data=data[:4]):
                result = self.run_checker(data)
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_a_ps1_with_non_ascii_text_needs_the_pwsh_7_requirement(self) -> None:
        body = f'Write-Host "a {EM_DASH} b"\n'
        bare = self.run_checker(body.encode("utf-8"), "tool.ps1")
        self.assertEqual(bare.returncode, 1, bare.stdout)
        self.assertIn("tool.ps1:1:", bare.stdout)
        self.assertIn("#Requires -Version 7", bare.stdout)
        for header in ("#Requires -Version 7\n", "#requires -version 7.4\r\n"):
            with self.subTest(header=header):
                declared = self.run_checker((header + body).encode("utf-8"), "tool.ps1")
                self.assertEqual(declared.returncode, 0, declared.stdout)

    def test_an_ascii_ps1_and_a_non_ps1_pass_without_the_requirement(self) -> None:
        self.assertEqual(self.run_checker(b'Write-Host "ok"\n', "tool.ps1").returncode, 0)
        self.assertEqual(self.run_checker(f"x {EM_DASH}\n".encode("utf-8"), "notes.md").returncode, 0)
        old = self.run_checker(b"#Requires -Version 5.1\n" + f"x {EM_DASH}\n".encode("utf-8"), "tool.ps1")
        self.assertEqual(old.returncode, 1, old.stdout)


class HookSelectionTest(unittest.TestCase):
    """#4333: the pre-commit hook runs on a UTF-16 file that identify tags as binary, and on no real binary."""

    @staticmethod
    def hook() -> tuple[set[str], re.Pattern[str]]:
        config = (REPO / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        block = re.search(r"- id: check-utf8\n(.*?)(?=\n\s*- id: )", config, re.S)
        assert block is not None, "the check-utf8 hook is in .pre-commit-config.yaml"
        types = re.search(r"^\s*types_or: \[([^\]]*)\]", block.group(1), re.M)
        exclude = re.search(r"^\s*exclude: '([^']*)'", block.group(1), re.M)
        assert types is not None and exclude is not None
        return {t.strip() for t in types.group(1).split(",")}, re.compile(exclude.group(1))

    def test_binary_tagged_files_are_selected(self) -> None:
        types, _ = self.hook()
        self.assertTrue({"text", "binary"} <= types, types)

    def test_the_real_binaries_are_excluded_and_the_text_inputs_are_not(self) -> None:
        _, exclude = self.hook()
        for path in ("assets/icons/tasksmack.png", "docs/images/processes.webp", "assets/tasksmack.ico",
                     "assets/fonts/Inter.ttf", "tests/fuzz/corpus/protocol/seed-1"):
            self.assertIsNotNone(exclude.search(path), path)
        for path in ("assets/tasksmack.rc.in", "tools/wpr/tasksmack.wprp", "packaging/linux/tasksmack.policy",
                     "packaging/linux/tasksmack.desktop", "src/main.cpp"):
            self.assertIsNone(exclude.search(path), path)

    def test_a_utf16_rc_in_is_tagged_binary_selected_and_fails(self) -> None:
        try:
            from identify import identify  # pre-commit's own file-type library
        except ImportError:
            self.skipTest("identify isn't installed (it comes with pre-commit)")
        types, exclude = self.hook()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "tasksmack.rc.in"
            path.write_bytes("\ufeff1 VERSIONINFO\r\n".encode("utf-16-le"))
            tags = identify.tags_from_path(str(path))
            self.assertIn("binary", tags, "the case the old `types: [text]` skipped")
            self.assertTrue(tags & types and exclude.search("assets/tasksmack.rc.in") is None)
            self.assertIn("UTF-16 LE byte-order mark", checker.check_file(str(path))[0])

    def test_no_tracked_binary_reaches_the_hook(self) -> None:
        try:
            from identify import identify
        except ImportError:
            self.skipTest("identify isn't installed (it comes with pre-commit)")
        _, exclude = self.hook()
        listed = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout
        for name in filter(None, listed.decode("utf-8").split("\0")):
            path = REPO / name
            if path.is_symlink() or not path.is_file() or exclude.search(name):
                continue
            if "binary" in identify.tags_from_path(str(path)):
                self.fail(f"{name} is binary but not excluded from check-utf8")


if __name__ == "__main__":
    unittest.main()
