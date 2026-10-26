#!/usr/bin/env python3
"""Tests for tools/profile-heap.sh's benchmark selection (#2987).

Google Benchmark exits 0 when --benchmark_filter matches nothing, and heaptrack then records only its startup
and registration as though it were the requested workload. The script runs here from a copy in a scratch
checkout against a stub benchmark binary and a stub heaptrack that records being started, so nothing is
profiled. Needs bash; Linux only, as the script is.
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

# Lists BM_Real for any filter but one naming "NoSuch", for which it complains on stderr, as Google Benchmark does.
STUB_BENCHMARKS = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    for arg in "$@"; do
        case "$arg" in
            --benchmark_filter=*NoSuch*) filter_misses=1 ;;
            --benchmark_list_tests=true) list=1 ;;
        esac
    done
    if [[ -n "${list:-}" ]]; then
        if [[ -n "${filter_misses:-}" ]]; then
            echo "Failed to match any benchmarks against regex: ^BM_NoSuch$" >&2
        else
            echo "BM_Real"
        fi
    fi
    exit 0
    """
)
# Records its arguments and leaves an output file where heaptrack would.
STUB_HEAPTRACK = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    echo "$@" >> "$HEAPTRACK_CALLS"
    [[ "$1" == -o ]] && : > "$2.1234.gz"
    exit 0
    """
)


class ProfileHeapTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="profile_heap_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "tools").mkdir()
        for name in ("profile-heap.sh", "common.sh"):
            shutil.copy2(REPO_ROOT / "tools" / name, self.root / "tools" / name)
        binary = self.root / "build" / "profile" / "bin" / "TaskSmackBenchmarks"
        binary.parent.mkdir(parents=True)
        binary.write_text(STUB_BENCHMARKS, encoding="utf-8")
        binary.chmod(0o755)
        bin_dir = self.root / "stub-bin"
        bin_dir.mkdir()
        (bin_dir / "heaptrack").write_text(STUB_HEAPTRACK, encoding="utf-8")
        (bin_dir / "heaptrack").chmod(0o755)
        self.calls = self.root / "heaptrack-calls.txt"
        self.env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "HOME": str(self.root),
            "HEAPTRACK_CALLS": str(self.calls),
        }

    def profile(self, bench_filter):
        return subprocess.run(
            [BASH, str(self.root / "tools" / "profile-heap.sh"), "bench", "--preset", "profile", "--skip-build",
             "--bench-filter", bench_filter, "--bench-reps", "1", "--bench-min-time", "0.01s"],
            capture_output=True,
            text=True,
            env=self.env,
            cwd=self.root,
            check=False,
        )

    def test_a_filter_matching_no_benchmark_fails_before_capturing(self):
        result = self.profile("^BM_NoSuch$")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("matches no benchmarks", result.stderr)
        self.assertFalse(self.calls.exists(), "heaptrack must not have been started")
        self.assertNotIn("Capture complete", result.stdout)

    def test_a_filter_that_matches_is_captured(self):
        result = self.profile("^BM_Real$")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.calls.exists())
        self.assertIn("--benchmark_filter=^BM_Real$", self.calls.read_text(encoding="utf-8"))
        self.assertIn("Capture complete", result.stdout)


if __name__ == "__main__":
    unittest.main()
