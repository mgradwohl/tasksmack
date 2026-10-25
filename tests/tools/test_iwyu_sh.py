#!/usr/bin/env python3
"""Tests for tools/iwyu.sh's verdict on translation units IWYU didn't analyze (#2666).

include-what-you-use that crashes on a translation unit (an assertion, a compile error) prints no
suggestions for it, so a script that only greps for suggestions counts it as clean. iwyu.sh runs
here from a copy in a scratch checkout, against stub iwyu_tool.py and include-what-you-use, so
nothing real is analyzed. Needs bash; registered in CTest on Linux only, like the script's CI job.
"""

import json
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
IWYU_SH = REPO_ROOT / "tools" / "iwyu.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

# Analyzes each .cpp named before "--": a name with "Crash" fails as IWYU's assertion does, one with
# "Suggest" gets suggestions, and any other is clean. Arguments after "--" are IWYU's own.
STUB_IWYU_TOOL = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    [[ "$1" == --help ]] && exit 0
    status=0
    while [[ $# -gt 0 && "$1" != -- ]]; do
        case "$1" in
            -p|-j) shift 2; continue ;;
            *Crash*.cpp) echo "iwyu.cc:1967: Assertion failed: TODO(csilvers): for objc and clang lang extensions"; status=1 ;;
            *Suggest*.cpp)
                printf '%s should add these lines:\\n#include <vector>\\n\\n' "$1"
                printf '%s should remove these lines:\\n\\n' "$1"
                printf 'The full include-list for %s:\\n#include <vector>\\n---\\n' "$1" ;;
            *.cpp) printf '(%s has correct #includes/fwd-decls)\\n' "$1" ;;
            *) echo "Skipping $1: not found in compilation database" ;;
        esac
        shift
    done
    exit "$status"
    """
)
STUB_IWYU = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    echo "include-what-you-use 0.26 based on clang version 22.1.2"
    """
)


class IwyuShTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="iwyu-sh-test-"))
        self.addCleanup(shutil.rmtree, self.root)
        (self.root / "tools").mkdir()
        shutil.copy(IWYU_SH, self.root / "tools" / "iwyu.sh")
        (self.root / ".iwyu.imp").write_text("[]\n", encoding="utf-8")
        src = self.root / "src"
        src.mkdir()
        for name in ("Clean.cpp", "Crash.cpp", "Suggest.cpp", "Header.h"):
            (src / name).write_text("", encoding="utf-8")
        build = self.root / "build" / "debug"
        build.mkdir(parents=True)
        (build / "build.ninja").write_text("", encoding="utf-8")
        entries = [
            {"directory": str(build), "file": str(src / name), "command": f"clang++ -std=c++23 -c {src / name}"}
            for name in ("Clean.cpp", "Crash.cpp", "Suggest.cpp")
        ]
        (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        for name, body in (("iwyu_tool.py", STUB_IWYU_TOOL), ("include-what-you-use", STUB_IWYU)):
            (bin_dir / name).write_text(body, encoding="utf-8")
            (bin_dir / name).chmod(0o755)
        self.summary = self.root / "step-summary.md"
        self.env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "HOME": str(self.root),
            "GITHUB_STEP_SUMMARY": str(self.summary),
        }

    def run_iwyu(self, *args):
        return subprocess.run(
            [BASH, str(self.root / "tools" / "iwyu.sh"), *args],
            capture_output=True,
            text=True,
            env=self.env,
            check=False,
        )

    def test_clean_translation_units_pass(self):
        result = self.run_iwyu("debug", "src/Clean.cpp", "src/Header.h")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("IWYU completed successfully - no issues found.", result.stdout)
        self.assertNotIn("did not analyze", result.stdout)

    def test_a_crashed_translation_unit_fails_and_is_named(self):
        result = self.run_iwyu("debug", "src/Clean.cpp", "src/Crash.cpp")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("IWYU did not analyze 1 translation unit(s)", result.stdout)
        self.assertIn("  src/Crash.cpp\n", result.stdout)
        self.assertNotIn("completed successfully", result.stdout)

    def test_report_mode_names_it_without_failing_and_summarizes_it(self):
        result = self.run_iwyu("--report", "debug", "src/Clean.cpp", "src/Crash.cpp")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("  src/Crash.cpp\n", result.stdout)
        self.assertIn("(Report-only mode: not failing)", result.stdout)
        self.assertNotIn("completed successfully", result.stdout)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("IWYU did not analyze 1 translation unit(s)", summary)
        self.assertIn("- `src/Crash.cpp`", summary)

    def test_suggestions_count_as_analyzed(self):
        result = self.run_iwyu("--report", "debug", "src/Suggest.cpp")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("IWYU found include issues.", result.stdout)
        self.assertNotIn("did not analyze", result.stdout)
        self.assertFalse(self.summary.exists())

    def test_a_full_run_checks_every_database_entry(self):
        result = self.run_iwyu("--report", "debug")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("IWYU did not analyze 1 translation unit(s)", result.stdout)
        self.assertIn("  src/Crash.cpp\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
