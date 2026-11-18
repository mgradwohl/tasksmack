#!/usr/bin/env python3
"""Tests for tools/profile-perf.sh's bench mode and option checks (#4211, #4212, #4222, #4223, #4210), against
a stub perf and a stub TaskSmackBenchmarks, so nothing is profiled or built. Needs bash; Linux only, as the
script is. The rest of the script, app mode's lifecycle above all, is #4225's."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

# Lists BM_ProcessModel_Refresh, or two names for a filter naming "Several", as Google Benchmark lists tests.
STUB_BENCHMARKS = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    for arg in "$@"; do [[ "$arg" == --benchmark_list_tests=true ]] && list=1; done
    if [[ -n "${list:-}" ]]; then
        if [[ "$*" == *Several* ]]; then printf 'BM_A\\nBM_B\\n'; else echo "BM_ProcessModel_Refresh"; fi
    fi
    exit 0
    """
)

# perf: counts only cpu-clock (as WSL2), records its record arguments, writes -o's file, and exits
# $STUB_PERF_RECORD_EXIT from record; `script -G` prints $STUB_PERF_SAMPLES lines.
STUB_PERF = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    case "$1" in
        --version) echo "perf version 6.0.stub" ;;
        stat)
            [[ "$*" == *cpu-clock* ]] && echo "1.00,msec,cpu-clock,1000,100.00,,"
            exit 0 ;;
        record)
            shift
            printf '%s\\n' "$@" > "$STUB_PERF_CALLS"
            out=""
            while [[ $# -gt 0 ]]; do [[ "$1" == -o ]] && { out="$2"; break; }; shift; done
            [[ -n "$out" ]] && : > "$out"
            exit "${STUB_PERF_RECORD_EXIT:-0}" ;;
        script)
            for ((i = 0; i < ${STUB_PERF_SAMPLES:-3}; i++)); do echo "sample $i"; done ;;
    esac
    exit 0
    """
)


@unittest.skipUnless(sys.platform.startswith("linux"), "profile-perf.sh is Linux-only")
class ProfilePerfBenchTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="profile_perf_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "tools").mkdir()
        for name in ("profile-perf.sh", "common.sh"):
            shutil.copy2(REPO_ROOT / "tools" / name, self.root / "tools" / name)
        for preset in ("profile", "benchmark"):
            binary = self.root / "build" / preset / "bin" / "TaskSmackBenchmarks"
            binary.parent.mkdir(parents=True)
            binary.write_text(STUB_BENCHMARKS, encoding="utf-8")
            binary.chmod(0o755)
        (self.root / "build" / "profile" / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG -fno-omit-frame-pointer\n",
            encoding="utf-8",
        )
        bin_dir = self.root / "stub-bin"
        bin_dir.mkdir()
        (bin_dir / "perf").write_text(STUB_PERF, encoding="utf-8")
        (bin_dir / "perf").chmod(0o755)
        self.calls = self.root / "perf-calls.txt"
        self.env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "HOME": str(self.root),
            "STUB_PERF_CALLS": str(self.calls),
        }

    def run_perf(self, *args, env=None):
        return subprocess.run(
            [BASH, str(self.root / "tools" / "profile-perf.sh"), *args],
            capture_output=True, text=True, env={**self.env, **(env or {})}, cwd=self.root, check=False, timeout=60,
        )

    def bench(self, *args, env=None):
        return self.run_perf("bench", "--skip-build", "--bench-reps", "1", "--bench-min-time", "0.01s", *args, env=env)

    def test_the_defaults_are_the_profile_preset_one_benchmark_and_frame_pointers(self):
        # #4222, #4223 (as profile-etw.ps1, #4241), #4210: the profile preset keeps frame pointers, so fp.
        result = self.bench()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertIn(str(self.root / "build" / "profile" / "bin" / "TaskSmackBenchmarks"), calls)
        self.assertIn("--benchmark_filter=BM_ProcessModel_Refresh$", calls)
        self.assertEqual(calls[calls.index("--call-graph") + 1], "fp")
        self.assertIn("Capture complete", result.stdout)
        log = next((self.root / "perf-data").glob("perf-bench-*[0-9].log")).read_text(encoding="utf-8")
        self.assertIn("PERF_CALL_GRAPH=fp", log)
        self.assertIn("PERF_VERSION=perf version 6.0.stub", log)

    def test_a_build_without_frame_pointers_unwinds_by_dwarf_and_call_graph_overrides(self):
        result = self.bench("--preset", "benchmark")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(calls[calls.index("--call-graph") + 1], "dwarf,16384")
        result = self.bench("--call-graph", "lbr")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(calls[calls.index("--call-graph") + 1], "lbr")

    def test_a_failed_capture_says_so_and_never_complete(self):
        # #4211: it printed "Capture complete" and "Next steps", then exited non-zero.
        result = self.bench(env={"STUB_PERF_RECORD_EXIT": "3"})
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("the capture failed (exit 3)", result.stderr)
        self.assertNotIn("Capture complete", result.stdout)
        self.assertNotIn("Next steps", result.stdout)

    def test_several_matches_warn(self):
        result = self.bench("--bench-filter", "Several")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("matches 2 benchmarks", result.stderr)

    def test_options_of_the_other_mode_and_bad_values_are_refused_before_anything_runs(self):
        # #4212: an app capture used to run with a --bench-filter silently ignored.
        for args, message in (
            (("app", "--bench-filter", "X"), "applies to bench mode only"),
            (("app", "--bench-min-time", "1s"), "applies to bench mode only"),
            (("bench", "--warmup", "3"), "applies to app mode only"),
            (("bench", "--bench-reps", "0"), "--bench-reps must be"),
            (("bench", "--bench-min-time", "quick"), "--bench-min-time takes"),
            (("bench", "--call-graph", "nope"), "--call-graph takes"),
        ):
            with self.subTest(args=args):
                result = self.run_perf(*args, "--skip-build")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(message, result.stderr)
                self.assertFalse(self.calls.exists(), "perf record never ran")


if __name__ == "__main__":
    unittest.main()
