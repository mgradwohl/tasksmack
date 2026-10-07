#!/usr/bin/env python3
"""Tests for tools/bench.sh against a stub benchmark binary (#1423).

A failing or crashing benchmark fails the script with its own exit code, partial output is
redacted or deleted, output that cannot be redacted after exit 0 is deleted, an extra
--benchmark_out is refused, and a successful run is redacted. Needs bash; registered in CTest only
where bash is available.
"""

import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH_SH = REPO_ROOT / "tools" / "bench.sh"

STUB = r"""#!/usr/bin/env bash
out=""
for arg in "$@"; do
    case "${arg}" in --benchmark_out=*) out="${arg#--benchmark_out=}" ;; esac
done
body='{"context": {"host_name": "'"${STUB_HOST}"'", "executable": "/some/dir/TaskSmackBenchmarks"},
 "benchmarks": [{"name": "BM_X_median", "run_name": "BM_X", "aggregate_name": "median", "real_time": 11.0, "time_unit": "ns"}]}'
case "${STUB_OUTPUT}" in
    partial) printf '%s' "${body:0:60}" > "${out}" ;;
    *) printf '%s\n' "${body}" > "${out}" ;;
esac
exit "${STUB_EXIT}"
"""

# CTest passes the bash it found (TASKSMACK_TEST_BASH); on Windows a bare `bash` can be WSL's.
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash")


def posix(path: Path) -> str:
    """A path bash accepts on every platform (forward slashes; Git Bash maps C:/ itself)."""
    return str(path).replace("\\", "/")


def make_executable(path: Path) -> None:
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


@unittest.skipUnless(BASH, "bash not available")
class BenchShTest(unittest.TestCase):
    def setUp(self):
        # Under a non-ASCII directory name, so the paths the script handles hold Unicode characters.
        self._tmp = tempfile.TemporaryDirectory(prefix="tasksmack-bench-\u00fcn\u00efc\u00f8d\u00e9-")
        self.root = Path(self._tmp.name)
        self.stub = self.root / "bin" / "TaskSmackBenchmarks"
        self.stub.parent.mkdir()
        self.stub.write_text(STUB, encoding="utf-8", newline="\n")
        make_executable(self.stub)
        # bench.sh calls python3; point it at this interpreter (on Windows, python3 on PATH can be
        # the Microsoft Store alias).
        self.shim_dir = self.root / "shim"
        self.shim_dir.mkdir()
        shim = self.shim_dir / "python3"
        shim.write_text(f'#!/bin/sh\nexec "{posix(Path(sys.executable))}" "$@"\n', encoding="utf-8", newline="\n")
        make_executable(shim)

    def tearDown(self):
        self._tmp.cleanup()

    def run_bench(self, name: str, stub_exit: int, stub_output: str = "full", extra: tuple[str, ...] = ()):
        out_dir = self.root / name
        env = dict(os.environ)
        env.update(
            TASKSMACK_BENCH_BIN=posix(self.stub),
            TASKSMACK_BENCH_OUT_DIR=posix(out_dir),
            STUB_EXIT=str(stub_exit),
            STUB_OUTPUT=stub_output,
            STUB_HOST=socket.gethostname(),
            PATH=str(self.shim_dir) + os.pathsep + os.environ.get("PATH", ""),
        )
        result = subprocess.run(
            [BASH, posix(BENCH_SH), "fake", "--", "--benchmark_filter=BM_X", *extra],
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        return result.returncode, result.stdout + result.stderr, sorted(out_dir.glob("*.json"))

    def test_failing_benchmark_fails_the_script(self):
        code, output, results = self.run_bench("failed", 3)
        self.assertEqual(code, 3, output)
        self.assertIn("exited with code 3", output)
        self.assertNotIn("Results written to", output)
        # The parseable partial result is kept, redacted.
        self.assertEqual(len(results), 1, output)
        self.assertEqual(json.loads(results[0].read_text(encoding="utf-8"))["context"]["host_name"], "redacted")

    def test_crash_with_partial_output_deletes_it(self):
        code, output, results = self.run_bench("crashed", 5, "partial")
        self.assertEqual(code, 5, output)
        self.assertIn("exited with code 5", output)
        self.assertEqual(results, [], output)

    def test_unredactable_output_after_success_is_deleted(self):
        code, output, results = self.run_bench("garbled", 0, "partial")
        self.assertNotEqual(code, 0, output)
        self.assertIn("could not be redacted", output)
        self.assertEqual(results, [], output)

    def test_output_overrides_are_refused_before_launch(self):
        # Google Benchmark takes the last --benchmark_out(_format), so an extra one would write
        # somewhere the redaction never looks. A relative path, run from the root, so a file
        # written anyway would be found.
        for override in ("--benchmark_out=elsewhere.json", "--benchmark_out_format=csv"):
            with self.subTest(override=override):
                code, output, results = self.run_bench(f"override-{len(override)}", 0, extra=(override,))
                self.assertNotEqual(code, 0, output)
                self.assertIn("TASKSMACK_BENCH_OUT_DIR", output)
                self.assertEqual(results, [], output)
                self.assertFalse((self.root / "elsewhere.json").exists(), "the benchmark ran")

    def test_successful_run_is_redacted(self):
        code, output, results = self.run_bench("ok", 0)
        self.assertEqual(code, 0, output)
        self.assertIn("Results written to", output)
        self.assertEqual(len(results), 1, output)
        context = json.loads(results[0].read_text(encoding="utf-8"))["context"]
        self.assertEqual(context, {"host_name": "redacted", "executable": "TaskSmackBenchmarks"})


if __name__ == "__main__":
    unittest.main()
