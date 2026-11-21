#!/usr/bin/env python3
"""tools/ninja-build-profile.py (#1875, #4226): the build-profile summary CI appends to a job's summary
(reusable-build-test.yml and heavy-checks.yml). Tested from synthetic .ninja_log files: each output's kind, the
latest line kept per output, an edge with several outputs counted once, and a missing or empty log printing
nothing. Pure Python; runs anywhere."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "ninja-build-profile.py"

_spec = importlib.util.spec_from_file_location("ninja_build_profile", SCRIPT)
assert _spec is not None and _spec.loader is not None
profile = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(profile)


def ninja_log(*rows: tuple[int, int, str, str]) -> str:
    """A .ninja_log v6: start and end in ms, the output, and the command hash."""
    lines = ["# ninja log v6"]
    lines += [f"{start}\t{end}\t0\t{output}\t{cmd_hash}" for start, end, output, cmd_hash in rows]
    return "\n".join(lines) + "\n"


class KindTest(unittest.TestCase):
    def test_each_output_is_sorted_into_its_kind(self):
        cases = {
            "src/CMakeFiles/TaskSmackCore.dir/Core/Application.cpp.o": "compile",
            "src\\CMakeFiles\\App.dir\\main.cpp.obj": "compile",
            "TaskSmack.res": "compile",
            "src/CMakeFiles/TaskSmackCore.dir/Core/Application.cpp.o.ddi": "module scan",
            "src/libTaskSmackCore.a": "static library",
            "TaskSmackCore.lib": "static library",
            "bin/TaskSmack": "link",
            "bin/TaskSmack.exe": "link",
            "bin/glfw3.dll": "link",
            "lib/libfoo.so.1.2": "link",
            "tests/TaskSmackTests": "link",  # a Linux executable has no suffix
            "CMakeFiles/cmake.verify_globs": "other",
            "build.ninja": "other",
            "assets/themes/arctic-fire.toml": "other",
        }
        for output, expected in cases.items():
            with self.subTest(output=output):
                self.assertEqual(profile.kind(output), expected)


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.build = Path(tempfile.mkdtemp(prefix="ninja_profile_"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.build, ignore_errors=True))

    def summary(self, log: str | None, *args: str) -> subprocess.CompletedProcess[str]:
        if log is not None:
            (self.build / ".ninja_log").write_text(log, encoding="utf-8")
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(self.build), *args],
            capture_output=True, text=True, check=False, timeout=60,
        )

    def test_a_missing_or_empty_log_prints_nothing_and_succeeds(self):
        for log in (None, "", "# ninja log v6\n", "not\ta\tlog\n"):
            with self.subTest(log=log):
                result = self.summary(log)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_each_kind_is_counted_summed_and_its_longest_shown(self):
        result = self.summary(ninja_log(
            (0, 2000, "a.cpp.o", "h1"),
            (0, 4000, "b.cpp.o", "h2"),
            (4000, 4500, "libcore.a", "h3"),
            (4500, 9500, "bin/TaskSmack", "h4"),
        ))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("| compile | 2 | 6.0 | 4.0 |", result.stdout)
        self.assertIn("| static library | 1 | 0.5 | 0.5 |", result.stdout)
        self.assertIn("| link | 1 | 5.0 | 5.0 |", result.stdout)
        self.assertIn("| `bin/TaskSmack` | 5.0 |", result.stdout)

    def test_a_rebuilt_output_counts_only_its_latest_line(self):
        # .ninja_log is append-only: a reused build directory lists an output once per build that rebuilt it.
        result = self.summary(ninja_log(
            (0, 9000, "a.cpp.o", "old"),
            (0, 1000, "a.cpp.o", "new"),
        ))
        self.assertIn("| compile | 1 | 1.0 | 1.0 |", result.stdout)

    def test_an_edge_with_several_outputs_is_one_edge_of_its_most_specific_kind(self):
        # A link also writes its .pdb and import .lib, and a compile with a scan lists its .ddi: same times and hash.
        result = self.summary(ninja_log(
            (100, 3100, "bin/TaskSmack.exe", "link1"),
            (100, 3100, "bin/TaskSmack.pdb", "link1"),
            (100, 3100, "TaskSmack.lib", "link1"),
            (0, 700, "x.cpp.obj", "cc1"),
            (0, 700, "x.cpp.obj.ddi", "cc1"),
        ))
        self.assertIn("| link | 1 | 3.0 | 3.0 |", result.stdout)
        self.assertIn("| compile | 1 | 0.7 | 0.7 |", result.stdout)
        self.assertNotIn("static library", result.stdout)
        self.assertNotIn("module scan", result.stdout)
        self.assertIn("| `bin/TaskSmack.exe` | 3.0 |", result.stdout)

    def test_top_lists_only_the_slowest_links(self):
        rows = [(0, (i + 1) * 1000, f"bin/tool{i}", f"h{i}") for i in range(5)]
        result = self.summary(ninja_log(*rows), "--top", "2")
        self.assertIn("Slowest links (top 2):", result.stdout)
        self.assertIn("`bin/tool4`", result.stdout)
        self.assertIn("`bin/tool3`", result.stdout)
        self.assertNotIn("`bin/tool2`", result.stdout)


if __name__ == "__main__":
    unittest.main()
