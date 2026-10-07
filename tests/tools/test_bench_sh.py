#!/usr/bin/env python3
"""Tests for tools/bench.sh against a stub benchmark binary.

A failing run fails the script (#1423) with its partial output redacted or deleted, and every run
writes a provenance manifest (tools/bench-manifest.py) with the same keys as bench.ps1's and no
host or user name (#1424). Needs bash; registered in CTest only where bash is available.
"""

import getpass
import hashlib
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
 "benchmarks": [
  {"name": "BM_X", "run_name": "BM_X", "run_type": "iteration", "repetition_index": 0, "real_time": 10.0, "time_unit": "ns"},
  {"name": "BM_X", "run_name": "BM_X", "run_type": "iteration", "repetition_index": 1, "real_time": 12.0, "time_unit": "ns"},
  {"name": "BM_X_median", "run_name": "BM_X", "run_type": "aggregate", "aggregate_name": "median", "real_time": 11.0, "time_unit": "ns"}
 ]}'
case "${STUB_OUTPUT}" in
    partial) printf '%s' "${body:0:60}" > "${out}" ;;
    none) ;;
    *) printf '%s\n' "${body}" > "${out}" ;;
esac
exit "${STUB_EXIT}"
"""

EXPECTED_KEYS = {
    "schema_version",
    "generator",
    "created_utc",
    "preset",
    "result_file",
    "exit_code",
    "git",
    "binary",
    "build",
    "benchmark",
    "machine",
}
SECTION_KEYS = {
    "git": {"commit", "branch", "dirty"},
    "binary": {"name", "sha256"},
    "build": {
        "build_type",
        "generator",
        "compiler",
        "compiler_id",
        "compiler_version",
        "cxx_flags",
        "cxx_flags_config",
        "ipo",
    },
    "benchmark": {"args", "raw_repetitions", "report_aggregates_only"},
    "machine": {"label", "cpu_model", "logical_cores", "os_name", "os_version", "arch"},
}


def posix(path: Path) -> str:
    """A path bash accepts on every platform (forward slashes; Git Bash maps C:/ itself)."""
    return str(path).replace("\\", "/")


# CTest passes the bash it found (TASKSMACK_TEST_BASH); on Windows a bare `bash` can be WSL's.
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash")


@unittest.skipUnless(BASH, "bash not available")
class BenchShTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        build_dir = self.root / "build" / "fake"
        (build_dir / "bin").mkdir(parents=True)
        (build_dir / "CMakeFiles" / "4.0.0").mkdir(parents=True)
        (build_dir / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            f"CMAKE_CXX_COMPILER:FILEPATH=/home/{getpass.getuser()}/llvm/bin/clang++\n"
            "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n",
            encoding="utf-8",
        )
        (build_dir / "CMakeFiles" / "4.0.0" / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "22.1.8")\n', encoding="utf-8"
        )
        self.stub = build_dir / "bin" / "TaskSmackBenchmarks"
        self.stub.write_text(STUB, encoding="utf-8", newline="\n")
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        # bench.sh calls python3; point it at this interpreter (on Windows, python3 on PATH can be
        # the Microsoft Store alias).
        shim_dir = self.root / "shim"
        shim_dir.mkdir()
        shim = shim_dir / "python3"
        shim.write_text(f'#!/bin/sh\nexec "{posix(Path(sys.executable))}" "$@"\n', encoding="utf-8", newline="\n")
        shim.chmod(shim.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        self.shim_dir = shim_dir

    def tearDown(self):
        self._tmp.cleanup()

    def run_bench(self, name: str, stub_exit: int, stub_output: str = "full"):
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
            [BASH, posix(BENCH_SH), "fake", "--", "--benchmark_filter=BM_X"],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        results = sorted(p for p in out_dir.glob("*.json") if not p.name.endswith(".manifest.json"))
        manifests = sorted(out_dir.glob("*.manifest.json"))
        return result.returncode, result.stdout + result.stderr, results, manifests

    def test_failing_benchmark_fails_the_script(self):
        code, output, results, manifests = self.run_bench("failed", 3)
        self.assertEqual(code, 3, output)
        self.assertIn("exited with code 3", output)
        self.assertNotIn("Results written to", output)
        self.assertEqual(len(results), 1, output)
        self.assertEqual(json.loads(results[0].read_text(encoding="utf-8"))["context"]["host_name"], "redacted")
        self.assertEqual(len(manifests), 1, output)
        self.assertEqual(json.loads(manifests[0].read_text(encoding="utf-8"))["exit_code"], 3)

    def test_crash_with_partial_output_deletes_it(self):
        code, output, results, _ = self.run_bench("crashed", 5, "partial")
        self.assertEqual(code, 5, output)
        self.assertIn("exited with code 5", output)
        self.assertEqual(results, [], output)

    def test_manifest_has_provenance_and_no_identity(self):
        code, output, results, manifests = self.run_bench("ok", 0)
        self.assertEqual(code, 0, output)
        self.assertEqual(len(results), 1, output)
        self.assertEqual(len(manifests), 1, output)
        self.assertEqual(manifests[0].name, results[0].stem + ".manifest.json")

        data = json.loads(results[0].read_text(encoding="utf-8"))
        self.assertEqual(data["context"]["host_name"], "redacted")
        self.assertEqual(len([b for b in data["benchmarks"] if b.get("run_type") == "iteration"]), 2)

        text = manifests[0].read_text(encoding="utf-8")
        manifest = json.loads(text)
        self.assertEqual(set(manifest), EXPECTED_KEYS)
        for section, keys in SECTION_KEYS.items():
            self.assertEqual(set(manifest[section]), keys, section)
        self.assertEqual(manifest["generator"], "tools/bench.sh")
        self.assertEqual(manifest["exit_code"], 0)
        self.assertEqual(manifest["result_file"], results[0].name)
        self.assertRegex(manifest["git"]["commit"] or "", r"^[0-9a-f]{40}$")
        self.assertEqual(manifest["binary"]["name"], "TaskSmackBenchmarks")
        self.assertEqual(manifest["binary"]["sha256"], hashlib.sha256(self.stub.read_bytes()).hexdigest())
        self.assertEqual(manifest["build"]["build_type"], "Release")
        self.assertEqual(manifest["build"]["compiler"], "clang++")
        self.assertEqual(manifest["build"]["compiler_id"], "Clang")
        self.assertEqual(manifest["build"]["compiler_version"], "22.1.8")
        self.assertEqual(manifest["build"]["cxx_flags_config"], "-O3 -DNDEBUG")
        self.assertTrue(manifest["benchmark"]["raw_repetitions"])
        self.assertFalse(manifest["benchmark"]["report_aggregates_only"])
        args = manifest["benchmark"]["args"]
        self.assertNotIn("--benchmark_report_aggregates_only=true", args)
        self.assertIn("--benchmark_filter=BM_X", args)
        self.assertIn(f"--benchmark_out={results[0].name}", args)
        self.assertEqual(manifest["machine"]["logical_cores"], os.cpu_count())

        identities = {socket.gethostname(), getpass.getuser(), str(Path.home()), tempfile.gettempdir()}
        for identity in identities:
            if identity and len(identity) >= 3:
                self.assertNotIn(identity.lower(), text.lower(), f"manifest contains {identity!r}")
        self.assertNotRegex(text, r"host_?name|user_?name")


if __name__ == "__main__":
    unittest.main()
