#!/usr/bin/env python3
"""Tests for tools/check-utf8.py (#1648, #2529).

The pre-commit hook must refuse text that is not UTF-8, and text that was UTF-8 read as Windows-1252 or
Latin-1 and saved again ("mojibake"), such as the battery card's em dash that showed as U+00E2 plus two
garbage glyphs (#2529). Every non-ASCII sample here is built from code points, so this file stays
clean under the checker it tests.
"""

import importlib.util
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

    def run_checker(self, data: bytes) -> subprocess.CompletedProcess:
        path = self.tmp / "sample.cpp"
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


if __name__ == "__main__":
    unittest.main()
