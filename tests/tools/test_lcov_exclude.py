#!/usr/bin/env python3
"""Tests for tools/lcov-exclude.py, which drops untestable lines from the LCOV report (#2039, #3221).

Each case writes a small source file and an LCOV record for it, runs the script on the LCOV file the
way tools/coverage.sh does, and checks which DA/BRDA/FN records survive and that LF/LH, BRF/BRH and
FNF/FNH lose exactly what was dropped.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "lcov-exclude.py"

SOURCE = """\
int keep(int x)
{
    // LCOV_EXCL_START - unreachable (test)
    if (x < 0)
    {
        return 0;
    }
    // LCOV_EXCL_STOP
    return x; // LCOV_EXCL_LINE - also unreachable (test)
}
struct Key
{
    friend bool operator==(const Key&, const Key&) = default;
    bool operator<(const Key&) const;
    auto operator<=>(const Key&) const = default; // trailing comment
};
int dropped() { return 1; } // LCOV_EXCL_LINE - a function that starts on an excluded line
"""


def lcov_for(source: Path) -> str:
    return "\n".join(
        [
            "TN:",
            f"SF:{source}",
            "FN:1,keep",
            "FN:17,dropped",
            "FNDA:3,keep",
            "FNDA:0,dropped",
            "FNF:2",
            "FNH:1",
            "BRDA:4,0,0,0",
            "BRDA:4,0,1,3",
            "BRDA:13,0,0,2",
            "BRDA:13,0,1,-",
            "BRDA:14,0,0,1",
            "BRF:5",
            "BRH:3",
            "DA:1,3",
            "DA:4,3",
            "DA:6,0",
            "DA:9,3",
            "DA:13,2",
            "DA:14,1",
            "DA:15,2",
            "DA:17,0",
            "LF:8",
            "LH:6",
            "end_of_record",
            "",
        ]
    )


class LcovExcludeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, lcov: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-I", "-X", "utf8", str(SCRIPT), str(lcov)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )

    def records(self, lcov: Path) -> list[str]:
        return lcov.read_text(encoding="utf-8").splitlines()

    def test_drops_marked_lines_and_defaulted_comparisons(self) -> None:
        source = self.dir / "a.cpp"
        source.write_text(SOURCE, encoding="utf-8")
        lcov = self.dir / "coverage.lcov"
        lcov.write_text(lcov_for(source), encoding="utf-8")

        result = self.run_script(lcov)

        self.assertEqual(result.returncode, 0, result.stderr)
        # DA 4, 6 (the region), 9 (LCOV_EXCL_LINE), 13 and 15 (defaulted comparisons), 17 (the line marker).
        self.assertEqual(result.stdout.strip(), "6")
        out = self.records(lcov)
        self.assertEqual([r for r in out if r.startswith("DA:")], ["DA:1,3", "DA:14,1"])
        self.assertEqual([r for r in out if r.startswith("BRDA:")], ["BRDA:14,0,0,1"])
        self.assertEqual([r for r in out if r.startswith("FN")], ["FN:1,keep", "FNDA:3,keep", "FNF:1", "FNH:1"])
        # Reduced from llvm-cov's own totals: LF 8-6, LH 6-4 (4, 9, 13 and 15 were hit), BRF 5-4, BRH 3-2.
        self.assertIn("LF:2", out)
        self.assertIn("LH:2", out)
        self.assertIn("BRF:1", out)
        self.assertIn("BRH:1", out)
        # A non-defaulted comparison is an ordinary line.
        self.assertIn("DA:14,1", out)

    def test_leaves_a_file_without_exclusions_unchanged(self) -> None:
        source = self.dir / "plain.cpp"
        source.write_text("int f()\n{\n    return 1;\n}\n", encoding="utf-8")
        lcov = self.dir / "coverage.lcov"
        original = f"TN:\nSF:{source}\nDA:3,0\nLF:1\nLH:0\nend_of_record\n"
        lcov.write_text(original, encoding="utf-8")

        result = self.run_script(lcov)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "0")
        self.assertEqual(lcov.read_text(encoding="utf-8"), original)

    def test_keeps_records_for_a_source_it_cannot_read(self) -> None:
        lcov = self.dir / "coverage.lcov"
        original = f"TN:\nSF:{self.dir / 'missing.cpp'}\nDA:1,1\nLF:1\nLH:1\nend_of_record\n"
        lcov.write_text(original, encoding="utf-8")

        result = self.run_script(lcov)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lcov.read_text(encoding="utf-8"), original)

    def test_an_unclosed_region_fails(self) -> None:
        source = self.dir / "open.cpp"
        source.write_text("// LCOV_EXCL_START - never closed\nint x = 0;\n", encoding="utf-8")
        lcov = self.dir / "coverage.lcov"
        lcov.write_text(f"TN:\nSF:{source}\nDA:2,1\nLF:1\nLH:1\nend_of_record\n", encoding="utf-8")

        result = self.run_script(lcov)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("without a matching LCOV_EXCL_STOP", result.stderr)

    def test_a_wrapped_64_bit_count_is_a_hit(self) -> None:
        # llvm-cov can print a wrapped counter (2**64 - 6); coverage.ps1 once failed on it (#3241).
        source = self.dir / "wrapped.cpp"
        source.write_text("int a = 0; // LCOV_EXCL_LINE - test\nint b = 0;\n", encoding="utf-8")
        lcov = self.dir / "coverage.lcov"
        lcov.write_text(
            f"SF:{source}\nDA:1,18446744073709551610\nDA:2,1\nBRDA:1,0,0,18446744073709551610\nBRF:1\nBRH:1\n"
            "LF:2\nLH:2\nend_of_record\n",
            encoding="utf-8",
        )

        result = self.run_script(lcov)

        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.records(lcov)
        self.assertEqual([r for r in out if r.startswith(("DA:", "LF:", "LH:", "BRF:", "BRH:"))], ["DA:2,1", "BRF:0", "BRH:0", "LF:1", "LH:1"])

    def test_resets_its_counts_per_file(self) -> None:
        first = self.dir / "first.cpp"
        first.write_text("int a = 0; // LCOV_EXCL_LINE - test\nint b = 0;\n", encoding="utf-8")
        second = self.dir / "second.cpp"
        second.write_text("int c = 0;\nint d = 0;\n", encoding="utf-8")
        lcov = self.dir / "coverage.lcov"
        lcov.write_text(
            f"SF:{first}\nDA:1,1\nDA:2,1\nLF:2\nLH:2\nend_of_record\n"
            f"SF:{second}\nDA:1,1\nDA:2,0\nLF:2\nLH:1\nend_of_record\n",
            encoding="utf-8",
        )

        self.run_script(lcov)

        out = self.records(lcov)
        self.assertEqual(out[out.index(f"SF:{first}") :][:5], [f"SF:{first}", "DA:2,1", "LF:1", "LH:1", "end_of_record"])
        self.assertEqual(out[out.index(f"SF:{second}") :][:6], [f"SF:{second}", "DA:1,1", "DA:2,0", "LF:2", "LH:1", "end_of_record"])


if __name__ == "__main__":
    unittest.main()
