#!/usr/bin/env python3
"""Tests for tools/analyze-perf.sh's verdict on unresolved symbols (#2977).

A perf without libelf/DWARF (WSL's) reports TaskSmack's functions as bare addresses, and the script used to
print that as a normal "Top N functions" summary and exit 0. It runs here from a copy in a scratch checkout
against a stub perf that serves a canned report, so nothing is profiled. Needs bash; Linux only, as the
script is.
"""

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

# `perf report` prints $FAKE_PERF_REPORT; `perf version --build-options` says libelf and DWARF are off.
STUB_PERF = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    case "$1" in
        report) cat "$FAKE_PERF_REPORT" ;;
        version) printf '         libelf: [ OFF ]\\n          dwarf: [ OFF ]\\n      libunwind: [ OFF ]\\n' ;;
        *) exit 0 ;;
    esac
    """
)

UNRESOLVED = """\
     8.94%  TaskSmackBenchm  TaskSmackBenchmarks  [.] 0x00000000001d7f40
     6.78%  TaskSmackBenchm  TaskSmackBenchmarks  [.] 0x00000000001d7f62
     2.10%  TaskSmackBenchm  libc.so.6            [.] 0x00000000000a1b2c
     1.00%  TaskSmackBenchm  [kernel.kallsyms]    [k] 0xffffffff81000000
"""
RESOLVED = """\
     8.94%  TaskSmackBenchm  TaskSmackBenchmarks  [.] Domain::ProcessModel::publish
     6.78%  TaskSmackBenchm  TaskSmackBenchmarks  [.] std::vector<int>::push_back
     2.10%  TaskSmackBenchm  libc.so.6            [.] __memmove_avx_unaligned_erms
"""
PARTIAL = """\
     8.94%  TaskSmackBenchm  TaskSmackBenchmarks  [.] Domain::ProcessModel::publish
     6.78%  TaskSmackBenchm  TaskSmackBenchmarks  [.] 0x00000000001d7f62
"""
KERNEL_ONLY = """\
     1.00%  TaskSmackBenchm  [kernel.kallsyms]    [k] native_write_msr
"""


def with_call_graphs(entries: int, graph_lines: int = 30) -> str:
    """A -g report of @p entries TaskSmack functions, each followed by @p graph_lines call-graph lines."""
    out = []
    for i in range(entries):
        out.append(f"     {50.0 - i:.2f}%  TaskSmackBenchm  TaskSmackBenchmarks  [.] Domain::Fn{i:02d}\n")
        out.append("            |\n")
        out.append(f"            ---Domain::Fn{i:02d}\n")
        out.extend(f"               |--{1.0:.2f}%--Caller{i:02d}_{j}\n" for j in range(graph_lines))
        out.append("\n")
    return "".join(out)


class AnalyzePerfTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="analyze_perf_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "tools").mkdir()
        for name in ("analyze-perf.sh", "common.sh"):
            shutil.copy2(REPO_ROOT / "tools" / name, self.root / "tools" / name)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        (bin_dir / "perf").write_text(STUB_PERF, encoding="utf-8")
        (bin_dir / "perf").chmod(0o755)
        (self.root / "perf-data").mkdir()
        self.data = self.root / "perf-data" / "perf-bench-test.data"
        self.data.write_bytes(b"")
        self.report = self.root / "report.txt"
        self.env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "HOME": str(self.root),
            "FAKE_PERF_REPORT": str(self.report),
        }

    def analyze(self, report, *args):
        self.report.write_text(report, encoding="utf-8")
        return subprocess.run(
            [BASH, str(self.root / "tools" / "analyze-perf.sh"), str(self.data), "--skip-flamegraph", *args],
            capture_output=True,
            text=True,
            env=self.env,
            cwd=self.root,
            check=False,
        )

    def test_all_unresolved_application_symbols_fail_and_say_why(self):
        result = self.analyze(UNRESOLVED)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("all 2 of TaskSmack's sampled functions are unresolved addresses", result.stderr)
        self.assertIn("libelf: [ OFF ]", result.stderr)
        self.assertIn("--allow-unresolved", result.stderr)
        self.assertNotIn("Top 20 functions", result.stdout)

    def test_allow_unresolved_keeps_the_address_only_report_with_a_warning(self):
        result = self.analyze(UNRESOLVED, "--allow-unresolved")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARNING: all 2 of TaskSmack's sampled functions are unresolved", result.stderr)
        self.assertIn("Unresolved: 2 of TaskSmack's 2 sampled functions are bare addresses.", result.stdout)
        self.assertIn("1 system-library rows have no symbols", result.stdout)

    def test_resolved_symbols_pass_quietly(self):
        result = self.analyze(RESOLVED)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Domain::ProcessModel::publish", result.stdout)
        self.assertNotIn("WARNING", result.stderr)
        self.assertNotIn("Unresolved:", result.stdout)

    def test_partly_unresolved_symbols_warn_but_pass(self):
        result = self.analyze(PARTIAL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARNING: 1 of TaskSmack's 2 sampled functions are unresolved addresses.", result.stderr)

    def test_top_n_lists_n_functions_and_the_report_keeps_their_whole_call_graphs(self):
        # #4208: the report was cut to TOP*10+50 lines, and Top N read from it: with call graphs a Top 20
        # listed a handful, and the report ended mid-graph.
        result = self.analyze(with_call_graphs(30), "--top", "20")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        top = result.stdout.split("Top 20 functions (self CPU):", 1)[1]
        listed = [line for line in top.splitlines() if "Domain::Fn" in line]
        self.assertEqual(len(listed), 20, result.stdout)
        report = next(self.data.parent.glob("*-report.txt")).read_text(encoding="utf-8")
        self.assertIn("Domain::Fn19", report)
        self.assertIn("Caller19_29", report, "the 20th entry's whole call graph")
        self.assertNotIn("Domain::Fn20", report, "and no 21st entry")

    def test_no_user_space_rows_says_so(self):
        result = self.analyze(KERNEL_ONLY)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("(No user-space functions sampled)", result.stdout)


if __name__ == "__main__":
    unittest.main()
