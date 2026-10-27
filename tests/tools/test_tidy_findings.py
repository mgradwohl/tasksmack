#!/usr/bin/env python3
"""Tests for tools/tidy-findings.py: the clang-tidy job summary (#1785), including its result-cache line, which
says which baseline was restored, or why none was, and how many translation units were reused (#3101)."""

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "tidy-findings.py"
spec = importlib.util.spec_from_file_location("tidy_findings", SCRIPT)
findings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(findings)

KEY = "Linux-tidy-results-40277744615-1"
COUNTS = (
    "clang-tidy cache: 150 of 152 translation units unchanged since a clean run (skipped), "
    "2 to analyze (0 not cacheable)\n"
)


class CacheLineTests(unittest.TestCase):
    def test_restored_baseline_and_counts(self):
        line = findings.cache_line(COUNTS, KEY, fresh=False)
        self.assertEqual(
            line, f"**Result cache:** baseline `{KEY}`; 152 selected, 150 reused, 2 analyzed."
        )

    def test_no_baseline_says_why(self):
        text = (
            "clang-tidy cache: 0 of 152 translation units unchanged since a clean run (skipped), "
            "152 to analyze (0 not cacheable)\n"
        )
        line = findings.cache_line(text, "", fresh=False)
        self.assertIn("baseline none found", line)
        self.assertIn("evicted", line)
        self.assertIn("152 selected, 0 reused, 152 analyzed", line)
        # The "nothing reused" hint is for a baseline that was restored but matched nothing.
        self.assertNotIn("nothing reused", line)

    def test_fresh_run(self):
        line = findings.cache_line(COUNTS.replace("150 of", "0 of").replace("2 to", "152 to"), "", fresh=True)
        self.assertIn("baseline none restored (a fresh run", line)
        self.assertIn("152 selected, 0 reused, 152 analyzed", line)

    def test_restored_baseline_that_matched_nothing_explains(self):
        text = "clang-tidy cache: 0 of 159 translation units unchanged since a clean run (skipped), 159 to analyze (0 not cacheable)"
        line = findings.cache_line(text, "Windows-tidy-results-1-1", fresh=False)
        self.assertIn("159 selected, 0 reused, 159 analyzed (nothing reused: a new runner image", line)

    def test_every_file_reused(self):
        text = (
            "clang-tidy cache: 7 of 7 translation units unchanged since a clean run (skipped), "
            "0 to analyze (0 not cacheable)\nclang-tidy completed successfully (every file unchanged since a clean run).\n"
        )
        self.assertIn("7 selected, 7 reused, 0 analyzed.", findings.cache_line(text, KEY, fresh=False))

    def test_unavailable_cache(self):
        text = "Warning: clang-tidy result cache unavailable; analyzing every file.\n"
        self.assertIn("couldn't key the files", findings.cache_line(text, KEY, fresh=False))

    def test_no_counts(self):
        self.assertIn("no cache counts", findings.cache_line("", KEY, fresh=False))

    def test_windows_output_with_colour_codes(self):
        # pwsh can wrap the passed-through stderr line in colour codes.
        text = "\x1b[31;1m" + COUNTS.rstrip("\n") + "\x1b[0m\r\n"
        self.assertIn("152 selected, 150 reused, 2 analyzed", findings.cache_line(text, KEY, fresh=False))


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="tidy-findings-test-"))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.output = self.dir / "clang-tidy-output.txt"
        self.output.write_text(
            COUNTS
            + "/src/A.cpp:1:2: warning: bad thing [readability-foo]\n"
            + "/src/A.cpp:1:2: warning: bad thing [readability-foo]\n"
            + "/src/B.cpp:3:4: error: worse thing [bugprone-bar,-warnings-as-errors]\n",
            encoding="utf-8",
        )

    def run_script(self, *args: str) -> str:
        result = subprocess.run(
            [sys.executable, "-I", "-X", "utf8", str(SCRIPT), str(self.output), "--title", "T", *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=True,
        )
        return result.stdout

    def test_without_cache_key_prints_no_cache_line(self):
        out = self.run_script()
        self.assertNotIn("Result cache", out)
        self.assertTrue(out.startswith("### T\n\n**2** unique finding(s) from 2 check(s):"))
        self.assertIn("| `readability-foo` | 1 |", out)
        self.assertIn("| `bugprone-bar` | 1 |", out)

    def test_cache_line_goes_under_the_title(self):
        out = self.run_script("--cache-key", KEY, "--cache-fresh", "false")
        self.assertTrue(
            out.startswith(f"### T\n\n**Result cache:** baseline `{KEY}`; 152 selected, 150 reused, 2 analyzed.\n\n**2**"),
            out,
        )

    def test_empty_cache_key_means_none_found(self):
        out = self.run_script("--cache-key", "", "--cache-fresh", "false")
        self.assertIn("baseline none found", out)

    def test_missing_output(self):
        self.output.unlink()
        out = self.run_script("--cache-key", KEY)
        self.assertEqual(out, "### T\n\nNo clang-tidy output (the step didn't run).\n")


if __name__ == "__main__":
    unittest.main()
