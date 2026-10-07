#!/usr/bin/env python3
"""Tests for tools/bench.sh against a stub benchmark binary.

A failing run fails the script (#1423) with its partial output redacted or deleted, and every run
writes a provenance manifest (tools/bench-manifest.py) with the same keys as bench.ps1's and no
host or user name (#1424). Needs bash; registered in CTest only where bash is available.
"""

import getpass
import hashlib
import importlib.util
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


def flag_forms(user: str, home: str) -> list[tuple[str, str]]:
    """(input, expected) pairs: every absolute-path form the scrubber handles, each holding the
    user name, and the prefix maps quoted every way (#1445 review); the same list as
    tools/test-bench.ps1. -DBUILT_BY=<user name> is no path: the final identity pass catches it
    (for a user name of at least 3 characters)."""
    repo = posix(REPO_ROOT)
    return [
        ("-fms-compatibility", "-fms-compatibility"),
        (f"-IC:/Users/{user}/a/inc", "-I<abs>/inc"),
        (f"-isystemC:\\Users\\{user}\\b\\inc", "-isystem<abs>/inc"),
        (f"-idirafter\\\\fileserver\\Users\\{user}\\c\\inc", "-idirafter<abs>/inc"),
        (f"-iquote//fileserver/Users/{user}/d/inc", "-iquote<abs>/inc"),
        (f"-I//bench-host/Users/{user}/sdk/include", "-I<abs>/include"),
        (f"-imsvc\\\\?\\C:\\Users\\{user}\\e\\inc", "-imsvc<abs>/inc"),
        (f"/I\\\\.\\C:\\Users\\{user}\\f\\inc", "/I<abs>/inc"),
        (f"/I/home/{user}/g/inc", "/I<abs>/inc"),
        (f"/IC:\\Users\\{user}\\sdk\\include", "/I<abs>/include"),
        (f"-L/home/{user}/lib", "-L<abs>/lib"),
        (f"-B/Users/{user}/bin", "-B<abs>/bin"),
        ("--sysroot=/root/sysroot", "--sysroot=<abs>/sysroot"),
        (f"-fprofile-use=/home/{user}/p.profdata", "-fprofile-use=<abs>/p.profdata"),
        (f"-fprofile-instr-use=C:/Users/{user}/q.profdata", "-fprofile-instr-use=<abs>/q.profdata"),
        (f"-fprofile-use /home/{user}/r.profdata", "-fprofile-use <abs>/r.profdata"),
        (f"-fprofile-use C:\\Users\\{user}\\pgo\\other.profdata", "-fprofile-use <abs>/other.profdata"),
        (f"-fdebug-prefix-map={home}/src=/src", "-fdebug-prefix-map=<abs>/src=/src"),
        (f"-ffile-prefix-map=C:/Users/{user}/src=//buildhost/Users/{user}/out", "-ffile-prefix-map=<abs>/src=<abs>/out"),
        (f"-isystem /opt/{user}/include", "-isystem <abs>/include"),
        (f'-I"C:/Users/{user}/My Includes/inc"', '-I"<abs>/inc"'),
        (f'"-isystem/home/{user}/with space/inc"', '"-isystem<abs>/inc"'),
        ("-I~/sdk/include", "-I<abs>/include"),
        (f"-I ~{user}/sdk/include", "-I <abs>/include"),
        (f'-fprofile-instr-use="{repo}/profiles/tasksmack.profdata"', '-fprofile-instr-use="<source>/profiles/tasksmack.profdata"'),
        ("/DWIN32 /W3 /EHsc -DNAME=value -std=c++23 /std:c++latest -O3", "/DWIN32 /W3 /EHsc -DNAME=value -std=c++23 /std:c++latest -O3"),
        (f"-Wl,-rpath,/home/{user}/lib", "-Wl,-rpath,<abs>/lib"),
        ("-fsanitize-ignorelist=dir/x/y.txt", "-fsanitize-ignorelist=dir/x/y.txt"),
        (f"-ffile-prefix-map=/opt/{user}/source=/mapped/source", "-ffile-prefix-map=<abs>/source=<abs>/source"),
        (f'-ffile-prefix-map="/opt/{user}/source=/mapped/source"', '-ffile-prefix-map="<abs>/source=<abs>/source"'),
        (f'"-fdebug-prefix-map=/home/{user}/My Src=/build/out dir"', '"-fdebug-prefix-map=<abs>/My Src=<abs>/out dir"'),
        (f'-fmacro-prefix-map="/home/{user}/src dir=/out/dir"', '-fmacro-prefix-map="<abs>/src dir=<abs>/dir"'),
        (f"-fprofile-prefix-map='C:\\Users\\{user}\\a b=D:\\x\\y'", "-fprofile-prefix-map='<abs>/a b=<abs>/y'"),
        (f'-ffile-prefix-map=/home/{user}/a="/x/new dir"', '-ffile-prefix-map=<abs>/a="<abs>/new dir"'),
        (f"-DDATA=foo:C:/Users/{user}/data", "-DDATA=foo:<abs>/data"),
        (f"/LIBPATH:C:\\Users\\{user}\\lib", "/LIBPATH:<abs>/lib"),
        ("-B/root/bin/x", "-B<abs>/x"),
        (f"-DBUILT_BY={user}", "-DBUILT_BY=<user>" if len(user) >= 3 else f"-DBUILT_BY={user}"),
    ]


def write_build_tree(build_dir: Path, cache_version: str, compilers: dict[str, str], flags: str = "") -> None:
    """A fake build/<preset> tree: CMakeCache.txt for CMake cache_version, and one
    CMakeFiles/<version>/CMakeCXXCompiler.cmake per entry of compilers (CMake version -> Clang
    version), as a tree reconfigured by several CMake versions keeps."""
    (build_dir / "bin").mkdir(parents=True)
    major, minor, patch = cache_version.split(".")
    (build_dir / "CMakeCache.txt").write_text(
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "CMAKE_GENERATOR:INTERNAL=Ninja\n"
        f"CMAKE_CXX_COMPILER:FILEPATH=/home/{getpass.getuser()}/llvm/bin/clang++\n"
        f"CMAKE_CXX_FLAGS:STRING={flags}\n"
        f'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -fprofile-instr-use="{posix(REPO_ROOT)}/profiles/tasksmack.profdata"'
        f" -fprofile-use=/home/{getpass.getuser()}/x.profdata\n"
        f"CMAKE_CACHE_MAJOR_VERSION:INTERNAL={major}\n"
        f"CMAKE_CACHE_MINOR_VERSION:INTERNAL={minor}\n"
        f"CMAKE_CACHE_PATCH_VERSION:INTERNAL={patch}\n",
        encoding="utf-8",
    )
    for cmake_version, clang_version in compilers.items():
        (build_dir / "CMakeFiles" / cmake_version).mkdir(parents=True)
        (build_dir / "CMakeFiles" / cmake_version / "CMakeCXXCompiler.cmake").write_text(
            f'set(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "{clang_version}")\n', encoding="utf-8"
        )


def write_stub(path: Path) -> Path:
    path.write_text(STUB, encoding="utf-8", newline="\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


@unittest.skipUnless(BASH, "bash not available")
class BenchShTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        build_dir = self.root / "build" / "fake"
        self.flag_forms = flag_forms(getpass.getuser(), posix(Path.home()))
        # Configured by two CMake versions, as a reused tree is: only the cache's own names the
        # compiler.
        write_build_tree(
            build_dir, "4.1.0", {"4.0.0": "21.1.0", "4.1.0": "22.1.8"}, " ".join(form for form, _ in self.flag_forms)
        )
        self.stub = write_stub(build_dir / "bin" / "TaskSmackBenchmarks")
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

    def run_bench(
        self, name: str, stub_exit: int, stub_output: str = "full", extra: tuple[str, ...] = (), binary: Path | None = None
    ):
        out_dir = self.root / name
        env = dict(os.environ)
        env.pop("BENCHMARK_REPORT_AGGREGATES_ONLY", None)
        env.update(
            TASKSMACK_BENCH_BIN=posix(binary or self.stub),
            TASKSMACK_BENCH_OUT_DIR=posix(out_dir),
            STUB_EXIT=str(stub_exit),
            STUB_OUTPUT=stub_output,
            STUB_HOST=socket.gethostname(),
            PATH=str(self.shim_dir) + os.pathsep + os.environ.get("PATH", ""),
        )
        result = subprocess.run(
            [BASH, posix(BENCH_SH), "fake", "--", "--benchmark_filter=BM_X", *extra],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        results = sorted(p for p in out_dir.glob("*.json") if not p.name.endswith(".manifest.json"))
        manifests = sorted(out_dir.glob("*.manifest.json"))
        return result.returncode, result.stdout + result.stderr, results, manifests

    def test_output_overrides_are_refused_before_launch(self):
        # #1445 review: Google Benchmark takes the last --benchmark_out(_format), so an extra one
        # would write somewhere the redaction and the manifest never look.
        elsewhere = self.root / "elsewhere.json"
        for override in (f"--benchmark_out={posix(elsewhere)}", "--benchmark_out_format=csv"):
            with self.subTest(override=override):
                code, output, results, manifests = self.run_bench(
                    f"override-{len(override)}", 0, extra=(override,)
                )
                self.assertNotEqual(code, 0, output)
                self.assertIn("TASKSMACK_BENCH_OUT_DIR", output)
                self.assertFalse(elsewhere.exists(), f"the benchmark ran and wrote {elsewhere}")
                self.assertEqual((results, manifests), ([], []), output)

    def test_manifest_records_the_effective_aggregates_setting(self):
        # #1445 review: the last --benchmark_report_aggregates_only wins, whichever way round.
        for extra, expected in (
            (("--benchmark_report_aggregates_only=true", "--benchmark_report_aggregates_only=FALSE"), False),
            (("--benchmark_report_aggregates_only=no", "--benchmark_report_aggregates_only"), True),
        ):
            with self.subTest(extra=extra):
                code, output, _, manifests = self.run_bench(f"aggregates-{expected}", 0, extra=extra)
                self.assertEqual(code, 0, output)
                benchmark = json.loads(manifests[0].read_text(encoding="utf-8"))["benchmark"]
                self.assertIs(benchmark["report_aggregates_only"], expected)
                self.assertIs(benchmark["raw_repetitions"], not expected)

    def test_a_compiler_directory_not_matching_the_cache_is_not_guessed(self):
        # #1445 review: the cache is CMake 4.2.0 and only a 4.0.0 directory exists.
        stale_dir = self.root / "build" / "stale"
        write_build_tree(stale_dir, "4.2.0", {"4.0.0": "21.1.0"})
        stub = write_stub(stale_dir / "bin" / "TaskSmackBenchmarks")
        code, output, _, manifests = self.run_bench("stale", 0, binary=stub)
        self.assertEqual(code, 0, output)
        build = json.loads(manifests[0].read_text(encoding="utf-8"))["build"]
        self.assertIsNone(build["compiler_id"])
        self.assertIsNone(build["compiler_version"])

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
        # Absolute paths in flags: the checkout's become <source>/..., others <abs>/<file name>.
        self.assertEqual(
            manifest["build"]["cxx_flags_config"],
            '-O3 -DNDEBUG -fprofile-instr-use="<source>/profiles/tasksmack.profdata" -fprofile-use=<abs>/x.profdata',
        )
        self.assertEqual(manifest["build"]["cxx_flags"], " ".join(expected for _, expected in self.flag_forms))
        self.assertTrue(manifest["benchmark"]["raw_repetitions"])
        self.assertFalse(manifest["benchmark"]["report_aggregates_only"])
        args = manifest["benchmark"]["args"]
        self.assertNotIn("--benchmark_report_aggregates_only=true", args)
        self.assertIn("--benchmark_filter=BM_X", args)
        self.assertIn(f"--benchmark_out={results[0].name}", args)
        self.assertEqual(manifest["machine"]["logical_cores"], os.cpu_count())

        identities = {
            socket.gethostname(),
            getpass.getuser(),
            str(Path.home()),
            posix(Path.home()),
            tempfile.gettempdir(),
            str(REPO_ROOT),
            posix(REPO_ROOT),
            "/home/",
        }
        for identity in identities:
            if identity and len(identity) >= 3:
                self.assertNotIn(identity.lower(), text.lower(), f"manifest contains {identity!r}")
        self.assertNotRegex(text, r"host_?name|user_?name")


def load_bench_manifest():
    spec = importlib.util.spec_from_file_location("bench_manifest", REPO_ROOT / "tools" / "bench-manifest.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ScrubberTest(unittest.TestCase):
    """tools/bench-manifest.py's flag scrubber and identity pass, without bash."""

    def test_every_form_is_scrubbed_exactly(self):
        module = load_bench_manifest()
        for given, expected in flag_forms("exampleuser", "/home/exampleuser"):
            if given.startswith("-DBUILT_BY="):
                continue  # the identity pass's job, below
            with self.subTest(given=given):
                self.assertEqual(module.hide_absolute_paths(given, REPO_ROOT), expected)

    def test_identity_pass_leaves_a_flag_word_user_name_alone(self):
        # #1445 review: a user named "build" must not mangle -DBUILD=1 and friends.
        module = load_bench_manifest()
        cases = [
            (
                "-DBUILD=1 -DBUILD_TYPE=Release -DCMAKE_BUILD=on --benchmark_filter=BM_Build",
                "-DBUILD=1 -DBUILD_TYPE=Release -DCMAKE_BUILD=on --benchmark_filter=BM_Build",
            ),
            ("-DBUILT_BY=build", "-DBUILT_BY=<user>"),
            ("E:/Users/build/x D:\\Users\\Build\\y", "E:/Users/<user>/x D:\\Users\\<user>\\y"),
            ("C:\\Users\\build\\src C:/Users/build/src", "<home>\\src <home>/src"),
        ]
        prefixes = ["C:\\Users\\build", "C:/Users/build"]
        for given, expected in cases:
            with self.subTest(given=given):
                self.assertEqual(module.hide_identity(given, prefixes, "build"), expected)
        # A user name under 3 characters is never replaced on its own; a home prefix always is.
        self.assertEqual(module.hide_identity("-DX=ab /home/ab/src", ["/home/ab"], "ab"), "-DX=ab <home>/src")


class ReportAggregatesOnlyTest(unittest.TestCase):
    """The manifest's effective --benchmark_report_aggregates_only, parsed as Google Benchmark does."""

    FLAG = "--benchmark_report_aggregates_only"

    def effective(self, *args: str, env: dict | None = None) -> bool:
        return load_bench_manifest().report_aggregates_only(list(args), env or {})

    def test_last_occurrence_wins_in_both_orders(self):
        self.assertFalse(self.effective(f"{self.FLAG}=true", f"{self.FLAG}=false"))
        self.assertTrue(self.effective(f"{self.FLAG}=false", f"{self.FLAG}=true"))

    def test_google_benchmark_truthiness(self):
        # IsTruthyFlagValue in google/benchmark src/commandlineflags.cc.
        for value in ("", "true", "TRUE", "1", "t", "T", "y", "Y", "yes", "on", "anything"):
            self.assertTrue(self.effective(f"{self.FLAG}={value}"), value)
        for value in ("false", "False", "0", "f", "F", "n", "N", "no", "NO", "off", "Off", "-"):
            self.assertFalse(self.effective(f"{self.FLAG}={value}"), value)
        self.assertTrue(self.effective(self.FLAG), "a bare flag is true")

    def test_default_and_environment(self):
        self.assertFalse(self.effective("--benchmark_filter=BM_X"))
        # Other flags sharing the prefix are not this flag.
        self.assertFalse(self.effective(f"{self.FLAG}_x=true"))
        self.assertTrue(self.effective(env={"BENCHMARK_REPORT_AGGREGATES_ONLY": "yes"}))
        self.assertFalse(self.effective(f"{self.FLAG}=0", env={"BENCHMARK_REPORT_AGGREGATES_ONLY": "1"}))


if __name__ == "__main__":
    unittest.main()
