#!/usr/bin/env python3
"""Tests for tools/bench.sh against a stub benchmark binary.

A failing run fails the script (#1423) with its partial output redacted or deleted, and every run
writes a provenance manifest (tools/bench-manifest.py) with the same keys as bench.ps1's and no
host or user name (#1424). Needs bash; registered in CTest only where bash is available.
"""

import datetime
import getpass
import hashlib
import importlib.util
import json
import os
import re
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
if [[ -n "${STUB_SLEEP:-}" ]]; then sleep "${STUB_SLEEP}"; fi
case "${STUB_OUTPUT}" in
    partial) printf '%s' "${body:0:60}" > "${out}" ;;
    none) ;;
    *) printf '%s\n' "${body}" > "${out}" ;;
esac
if [[ -n "${STUB_MUTATE:-}" ]]; then printf '# rebuilt during the run\n' >> "$0"; fi
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
        "cxx_flags_sha256",
        "cxx_flags_config_sha256",
        "cxx_flags_source",
        "ipo",
        "ipo_source",
    },
    "benchmark": {"args", "raw_repetitions", "report_aggregates_only"},
    "machine": {"label", "cpu_model", "logical_cores", "os_name", "os_version", "arch"},
}


def posix(path: Path) -> str:
    """A path bash accepts on every platform (forward slashes; Git Bash maps C:/ itself)."""
    return str(path).replace("\\", "/")


# CTest passes the bash it found (TASKSMACK_TEST_BASH); on Windows a bare `bash` can be WSL's.
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash")


# The identity pass's separators (tools/bench-manifest.py): a name only counts standing alone.
_SEPARATED = r"""\s/\\"'=:,;"""


def find_identity_leaks(value, tokens: list[str], paths: list[str], path: str = "") -> list[str]:
    """Every string value (never an object key) of a decoded manifest that still holds a token
    (user or host name) standing alone between the identity pass's separators, or contains one of
    the paths. Tokens under 3 characters are not checked, as the identity pass leaves them.

    The fields the writer exempts are skipped exactly as it skips them (IDENTITY_EXEMPT, and
    build.build_type when it is one of STANDARD_BUILD_TYPES, from tools/bench-manifest.py);
    machine.label is checked against the machine fields it is built from instead."""
    module = load_bench_manifest()
    if path == "machine.label" or path in module.IDENTITY_EXEMPT:
        return []
    if path == "build.build_type" and value in module.STANDARD_BUILD_TYPES:
        return []
    leaks = []
    if isinstance(value, dict):
        for key, item in value.items():
            leaks += find_identity_leaks(item, tokens, paths, f"{path}.{key}" if path else key)
        machine = value.get("machine") if not path else None
        if isinstance(machine, dict) and "label" in machine and machine["label"] != module.machine_label(machine):
            leaks.append(f"machine.label {machine['label']!r} is not built from the machine fields")
    elif isinstance(value, list):
        for item in value:
            leaks += find_identity_leaks(item, tokens, paths, path)
    elif isinstance(value, str):
        for token in tokens:
            pattern = rf"(?<![^{_SEPARATED}]){re.escape(token)}(?![^{_SEPARATED}])"
            if token and len(token) >= 3 and re.search(pattern, value, re.IGNORECASE):
                leaks.append(f"{token!r} in {path} {value!r}")
        leaks += [f"{item!r} in {path} {value!r}" for item in paths if item and item.lower() in value.lower()]
    return leaks


def raw_flags(user: str, home: str) -> tuple[str, str]:
    """Compiler flags holding user-home, profile and checkout paths, quotes and non-ASCII text, as
    (CMAKE_CXX_FLAGS, CMAKE_CXX_FLAGS_RELEASE). The manifest records only their SHA-256 (#1445),
    so none of this text may appear in it; the same strings as tools/test-bench.ps1."""
    return (
        f'-fms-compatibility -I"C:/Users/{user}/My Includes/inc" -isystem/home/{user}/sdk/include'
        f' -fdebug-prefix-map={home}/src=/src -DAPP_NAME=\\"TaskSmack\\" -DAUTHOR=Jos\u00e9',
        f'-O3 -DNDEBUG -fprofile-instr-use="{posix(REPO_ROOT)}/profiles/tasksmack.profdata" -fprofile-use={home}/x.profdata',
    )


FLAG_PROBES = ("-fms-compatibility", "My Includes", "sdk/include", "prefix-map", "APP_NAME", "tasksmack.profdata", "x.profdata", "-fprofile", "Jos\u00e9")


def write_build_tree(
    build_dir: Path, cache_version: str, compilers: dict[str, str], flags: str = "", config_flags: str = "-O3 -DNDEBUG"
) -> None:
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
        f"CMAKE_CXX_FLAGS_RELEASE:STRING={config_flags}\n"
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
        # Under a non-ASCII directory name (#1445 review), so every path the scripts handle holds
        # Unicode characters.
        self._tmp = tempfile.TemporaryDirectory(prefix="tasksmack-bench-\u00fcn\u00efc\u00f8d\u00e9-")
        self.root = Path(self._tmp.name)
        build_dir = self.root / "build" / "fake"
        self.flags, self.config_flags = raw_flags(getpass.getuser(), posix(Path.home()))
        # Configured by two CMake versions, as a reused tree is: only the cache's own names the
        # compiler.
        write_build_tree(build_dir, "4.1.0", {"4.0.0": "21.1.0", "4.1.0": "22.1.8"}, self.flags, self.config_flags)
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
        self,
        name: str,
        stub_exit: int,
        stub_output: str = "full",
        extra: tuple[str, ...] = (),
        binary: Path | str | None = None,
        script: Path = BENCH_SH,
        mutate: bool = False,
        leading: tuple[str, ...] = ("fake", "--"),
        cwd: Path | None = None,
        path_first: Path | None = None,
        cdpath: Path | None = None,
        env_extra: dict[str, str] | None = None,
    ):
        out_dir = self.root / name
        env = dict(os.environ)
        env.pop("BENCHMARK_REPORT_AGGREGATES_ONLY", None)
        env.update(
            TASKSMACK_BENCH_BIN=binary if isinstance(binary, str) else posix(binary or self.stub),
            TASKSMACK_BENCH_OUT_DIR=posix(out_dir),
            STUB_EXIT=str(stub_exit),
            STUB_OUTPUT=stub_output,
            STUB_HOST=socket.gethostname(),
            STUB_MUTATE="1" if mutate else "",
            PATH=os.pathsep.join(
                [*([str(path_first)] if path_first else []), str(self.shim_dir), os.environ.get("PATH", "")]
            ),
            # Every hop speaks UTF-8 whatever the runner's locale and code page: bash, its argument
            # conversion for native programs, and the Python helpers it starts.
            LC_ALL="C.UTF-8",
            PYTHONUTF8="1",
        )
        env.update(env_extra or {})
        if cdpath is not None:
            # CDPATH is ':'-separated and not converted by Git Bash, so a drive-letter path is given
            # in its /c/... form.
            value = posix(cdpath)
            env["CDPATH"] = f"/{value[0].lower()}{value[2:]}" if re.match(r"[A-Za-z]:/", value) else value
        result = subprocess.run(
            [BASH, posix(script), *leading, "--benchmark_filter=BM_X", *extra],
            env=env,
            cwd=cwd,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
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

    def test_provenance_is_captured_before_the_benchmark_starts(self):
        # #1445 review: the stub rewrites itself during the run, as a rebuild would; the manifest
        # keeps the hash of what was launched.
        launched = hashlib.sha256(self.stub.read_bytes()).hexdigest()
        code, output, _, manifests = self.run_bench("mutate", 0, mutate=True)
        self.assertEqual(code, 0, output)
        self.assertNotEqual(hashlib.sha256(self.stub.read_bytes()).hexdigest(), launched, "the stub did not change")
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        self.assertEqual(manifest["binary"]["sha256"], launched)
        self.assertEqual(manifest["exit_code"], 0)

    def test_a_bare_binary_name_is_the_file_in_the_current_directory_not_one_on_path(self):
        # #1445 review: TASKSMACK_BENCH_BIN=TaskSmackBenchmarks is checked and hashed as the file in
        # the current directory, so that file is what runs -- not a same-named program on PATH.
        decoy_dir = self.root / "decoy"
        decoy_dir.mkdir()
        ran = self.root / "decoy-ran"
        decoy = decoy_dir / "TaskSmackBenchmarks"
        decoy.write_text(f'#!/bin/sh\n: > "{posix(ran)}"\nexit 7\n', encoding="utf-8", newline="\n")
        decoy.chmod(decoy.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        code, output, results, manifests = self.run_bench(
            "bare-name", 0, binary="TaskSmackBenchmarks", cwd=self.stub.parent, path_first=decoy_dir
        )
        self.assertEqual(code, 0, output)
        self.assertFalse(ran.exists(), "the decoy on PATH was launched")
        self.assertEqual(len(results), 1, output)
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        self.assertEqual(manifest["binary"]["sha256"], hashlib.sha256(self.stub.read_bytes()).hexdigest())
        self.assertEqual(manifest["exit_code"], 0)

    def test_an_inherited_cdpath_does_not_change_the_binary(self):
        # #1445 review: with CDPATH set, `cd bin` would go to the CDPATH entry's bin/ and print it,
        # so the resolved binary path would be wrong (and two lines long).
        decoy_root = self.root / "cdpath"
        (decoy_root / "bin").mkdir(parents=True)
        code, output, results, manifests = self.run_bench(
            "cdpath", 0, binary="bin/TaskSmackBenchmarks", cwd=self.stub.parent.parent, cdpath=decoy_root
        )
        self.assertEqual(code, 0, output)
        self.assertEqual(len(results), 1, output)
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        self.assertEqual(manifest["binary"]["sha256"], hashlib.sha256(self.stub.read_bytes()).hexdigest())
        self.assertEqual(manifest["build"]["compiler_version"], "22.1.8")

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

    def test_a_preset_named_after_the_user_or_host_reaches_no_name(self):
        # #1445 review: '-' is no identity-token boundary, so a preset named after the user or the
        # machine survived as <preset>-<timestamp>.json in the file names, the manifest's
        # result_file and --benchmark_out. The names are built from the scrubbed preset instead.
        # The file names are checked for the names as plain substrings; the manifest with the
        # field-aware find_identity_leaks(), which skips the fields the writer keeps on purpose
        # (#1445 review): a user named Release keeps the Release build type, a host named Linux
        # the OS name. The "release" case runs as a user named Release (getpass reads USER and
        # LOGNAME first) to prove it.
        user = getpass.getuser()
        hosts = [socket.gethostname(), socket.gethostname().split(".")[0]]
        as_release = {name: "Release" for name in ("LOGNAME", "USER", "LNAME", "USERNAME")}
        for kind, preset, identities, env_extra in (
            ("user", user, [user], None),
            ("host", hosts[1], hosts, None),
            ("user", "Release", ["Release"], as_release),
        ):
            identities = [name for name in identities if len(name) >= 3]
            if len(preset) < 3 or preset.lower() in ("user", "host"):
                continue
            with self.subTest(preset=preset):
                code, output, results, manifests = self.run_bench(
                    f"preset-{preset}", 0, leading=(preset, "--"), env_extra=env_extra
                )
                self.assertEqual(code, 0, output)
                self.assertEqual((len(results), len(manifests)), (1, 1), output)
                manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
                self.assertEqual(manifest["preset"], f"<{kind}>")
                self.assertEqual(manifest["result_file"], results[0].name)
                self.assertTrue(results[0].name.startswith(f"{kind}-"), results[0].name)
                for name in (results[0].name, manifests[0].name):
                    for identity in identities:
                        self.assertNotIn(identity.lower(), name.lower(), f"{identity!r} in the file name {name!r}")
                self.assertEqual(find_identity_leaks(manifest, identities, []), [])
                if preset == "Release":
                    self.assertEqual(manifest["build"]["build_type"], "Release")

    def test_a_non_ascii_preset_with_a_non_utf8_python_stdout(self):
        # #1445 review: with a CP1252 stdout, writing the preset part of the file names as text
        # raised UnicodeEncodeError, and bench.sh stopped under set -e before the benchmark ran.
        preset = "benchmark-\u65e5\u672c\u8a9e"
        code, output, results, manifests = self.run_bench(
            "cp1252", 0, leading=(preset, "--"), env_extra={"PYTHONUTF8": "0", "PYTHONIOENCODING": "cp1252"}
        )
        self.assertEqual(code, 0, output)
        self.assertEqual((len(results), len(manifests)), (1, 1), output)
        self.assertTrue(results[0].name.startswith(preset + "-"), results[0].name)
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        self.assertEqual((manifest["preset"], manifest["result_file"]), (preset, results[0].name))

    def test_this_machines_host_name_in_the_args_does_not_survive(self):
        host = socket.gethostname()
        if len(host) < 3:
            self.skipTest("host name under 3 characters")
        code, output, _, manifests = self.run_bench("host", 0, extra=(f"--benchmark_context=tsk_ctx_machine={host}",))
        self.assertEqual(code, 0, output)
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        expected = "--benchmark_context=sha256:" + hashlib.sha256(f"tsk_ctx_machine={host}".encode("utf-8")).hexdigest()
        self.assertIn(expected, manifest["benchmark"]["args"])
        self.assertEqual(find_identity_leaks(manifest, [host, host.split(".")[0]], []), [])

    def test_a_multi_config_tree_keeps_the_benchmark_in_bin_config(self):
        # #1445 review: bin/<Config>/ under a multi-config generator; the cache has no build type.
        multi_dir = self.root / "build" / "multi"
        write_build_tree(multi_dir, "4.1.0", {"4.1.0": "22.1.8"})
        cache = multi_dir / "CMakeCache.txt"
        cache.write_text(
            cache.read_text(encoding="utf-8")
            .replace("CMAKE_BUILD_TYPE:STRING=Release", "CMAKE_BUILD_TYPE:STRING=")
            .replace("CMAKE_GENERATOR:INTERNAL=Ninja", "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config")
            + "CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG\n",
            encoding="utf-8",
        )
        (multi_dir / "bin" / "RelWithDebInfo").mkdir()
        stub = write_stub(multi_dir / "bin" / "RelWithDebInfo" / "TaskSmackBenchmarks")
        code, output, _, manifests = self.run_bench("multi", 0, binary=stub)
        self.assertEqual(code, 0, output)
        build = json.loads(manifests[0].read_text(encoding="utf-8"))["build"]
        self.assertEqual(build["build_type"], "RelWithDebInfo")
        self.assertEqual(build["generator"], "Ninja Multi-Config")
        self.assertEqual(build["cxx_flags_config_sha256"], hashlib.sha256(b"-O2 -g -DNDEBUG").hexdigest())
        self.assertEqual(build["compiler_version"], "22.1.8")

    def test_a_unicode_checkout_outside_git(self):
        # #1445 review: bench.sh copied into <root>/ch\u00e9ckout/tools; its own checkout maps to
        # nothing in the manifest (a --benchmark_context value naming it is hashed), its UTF-8 flags
        # hash as UTF-8, and the missing git repository leaves the git fields
        # unknown, not an error.
        checkout = self.root / "ch\u00e9ckout"
        (checkout / "tools").mkdir(parents=True)
        for name in ("bench.sh", "bench-manifest.py"):
            shutil.copy2(REPO_ROOT / "tools" / name, checkout / "tools" / name)
        build_dir = checkout / "build" / "uni"
        (build_dir / "bin").mkdir(parents=True)
        config_flags = f'-O3 -fprofile-instr-use="{posix(checkout)}/profiles/tasksmack.profdata" -DAUTHOR=Jos\u00e9'
        (build_dir / "CMakeCache.txt").write_text(
            f"CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS_RELEASE:STRING={config_flags}\n", encoding="utf-8"
        )
        stub = write_stub(build_dir / "bin" / "TaskSmackBenchmarks")
        profile_arg = f"--benchmark_context=tsk_ctx_profile={posix(checkout)}/profiles/tasksmack.profdata"
        code, output, _, manifests = self.run_bench(
            "uni", 0, binary=stub, script=checkout / "tools" / "bench.sh", extra=(profile_arg,)
        )
        self.assertEqual(code, 0, output)
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        self.assertEqual(manifest["build"]["cxx_flags_config_sha256"], hashlib.sha256(config_flags.encode("utf-8")).hexdigest())
        self.assertIn(
            "--benchmark_context=sha256:" + hashlib.sha256(profile_arg.split("=", 1)[1].encode("utf-8")).hexdigest(),
            manifest["benchmark"]["args"],
        )
        self.assertIsNone(manifest["git"]["commit"])
        self.assertIsNone(manifest["git"]["dirty"])
        # Self-review: in a repository on a non-ASCII branch, the branch name arrives intact (git
        # writes UTF-8 whatever the locale).
        if shutil.which("git") is None:
            return
        identity = ["-c", "user.name=bench-test", "-c", "user.email=bench-test@example.invalid"]
        # #1445 review: inside an enclosing repository (a source archive unpacked in another
        # checkout), git would find that repository; its provenance is not inherited.
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True, capture_output=True)
        subprocess.run(
            ["git", "-C", str(self.root), *identity, "commit", "-q", "--allow-empty", "-m", "outer"],
            check=True,
            capture_output=True,
        )
        code, output, _, manifests = self.run_bench("nested", 0, binary=stub, script=checkout / "tools" / "bench.sh")
        self.assertEqual(code, 0, output)
        self.assertEqual(
            json.loads(manifests[0].read_text(encoding="utf-8"))["git"], {"commit": None, "branch": None, "dirty": None}
        )
        branch = "fëature"
        subprocess.run(["git", "-C", str(checkout), "init", "-q", "-b", branch], check=True, capture_output=True)
        subprocess.run(
            ["git", "-C", str(checkout), *identity, "commit", "-q", "--allow-empty", "-m", "init"],
            check=True,
            capture_output=True,
        )
        code, output, _, manifests = self.run_bench("uni-branch", 0, binary=stub, script=checkout / "tools" / "bench.sh")
        self.assertEqual(code, 0, output)
        git = json.loads(manifests[0].read_text(encoding="utf-8"))["git"]
        self.assertEqual(git["branch"], branch)
        self.assertRegex(git["commit"] or "", r"^[0-9a-f]{40}$")
        self.assertIs(git["dirty"], False)

    def test_a_benchmark_flag_with_no_preset_uses_the_default_preset(self):
        # Self-review: ./tools/bench.sh --benchmark_filter=Foo, with no preset and no "--".
        code, output, results, manifests = self.run_bench("no-preset", 0, leading=())
        self.assertEqual(code, 0, output)
        self.assertEqual(len(results), 1, output)
        self.assertTrue(results[0].name.startswith("benchmark-"), results[0].name)
        args = json.loads(manifests[0].read_text(encoding="utf-8"))["benchmark"]["args"]
        self.assertIn("--benchmark_filter=BM_X", args)

    def test_unredactable_output_after_success_is_deleted(self):
        code, output, results, _ = self.run_bench("garbled", 0, "partial")
        self.assertNotEqual(code, 0, output)
        self.assertIn("could not be redacted", output)
        self.assertEqual(results, [], output)

    def test_concurrent_runs_claim_distinct_output_names(self):
        # #1445 review: four runs started together into one directory; the stub sleeps before
        # writing, so without an up-front claim they would all pick the same name.
        race = self.root / "race"
        env = dict(os.environ)
        env.pop("BENCHMARK_REPORT_AGGREGATES_ONLY", None)
        env.update(
            TASKSMACK_BENCH_BIN=posix(self.stub),
            TASKSMACK_BENCH_OUT_DIR=posix(race),
            STUB_EXIT="0",
            STUB_OUTPUT="full",
            STUB_SLEEP="0.4",
            STUB_HOST=socket.gethostname(),
            PATH=str(self.shim_dir) + os.pathsep + os.environ.get("PATH", ""),
            # Every hop speaks UTF-8 whatever the runner's locale and code page: bash, its argument
            # conversion for native programs, and the Python helpers it starts.
            LC_ALL="C.UTF-8",
            PYTHONUTF8="1",
        )
        racers = [
            subprocess.Popen(
                [BASH, posix(BENCH_SH), "fake", "--", "--benchmark_filter=BM_X"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            for _ in range(4)
        ]
        for racer in racers:
            racer.wait()
        results = sorted(p for p in race.glob("*.json") if not p.name.endswith(".manifest.json"))
        manifests = sorted(race.glob("*.manifest.json"))
        self.assertEqual((len(results), len(manifests)), (4, 4), [p.name for p in race.iterdir()])
        for manifest in manifests:
            claimed = json.loads(manifest.read_text(encoding="utf-8"))["result_file"]
            self.assertEqual(manifest.name, Path(claimed).stem + ".manifest.json")
            self.assertTrue((race / claimed).is_file(), claimed)

    def test_a_run_in_the_same_second_does_not_overwrite_an_earlier_one(self):
        # Results already sit under every name this run could pick in the next 30 seconds.
        collide = self.root / "collide"
        collide.mkdir()
        start = datetime.datetime.now()
        placeholders = []
        for offset in range(31):
            stamp = (start + datetime.timedelta(seconds=offset)).strftime("%Y%m%d-%H%M%S")
            path = collide / f"fake-{stamp}.json"
            path.write_text("placeholder", encoding="utf-8")
            placeholders.append(path)
        code, output, results, manifests = self.run_bench("collide", 0)
        self.assertEqual(code, 0, output)
        for path in placeholders:
            self.assertEqual(path.read_text(encoding="utf-8"), "placeholder", path)
        suffixed = [p for p in results if re.fullmatch(r"fake-\d{8}-\d{6}-2\.json", p.name)]
        self.assertEqual(len(suffixed), 1, [p.name for p in results])
        self.assertTrue(suffixed[0].with_name(suffixed[0].stem + ".manifest.json").is_file())

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
        data_arg = f"--benchmark_context=tsk_ctx_data={posix(Path.home())}/bench data/input.bin"
        code, output, results, manifests = self.run_bench("ok", 0, extra=(data_arg,))
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
        # Outside a git checkout (a source archive) or without git, the git fields are all unknown
        # by design; otherwise all known. Never a mix (#1445 review).
        git = manifest["git"]
        if git["commit"] is None:
            self.assertEqual(git, {"commit": None, "branch": None, "dirty": None})
        else:
            self.assertRegex(git["commit"], r"^[0-9a-f]{40}$")
            self.assertIsInstance(git["branch"], str)
            self.assertIsInstance(git["dirty"], bool)
        self.assertEqual(manifest["binary"]["name"], "TaskSmackBenchmarks")
        self.assertEqual(manifest["binary"]["sha256"], hashlib.sha256(self.stub.read_bytes()).hexdigest())
        self.assertEqual(manifest["build"]["build_type"], "Release")
        self.assertEqual(manifest["build"]["compiler"], "clang++")
        self.assertEqual(manifest["build"]["compiler_id"], "Clang")
        self.assertEqual(manifest["build"]["compiler_version"], "22.1.8")
        # The compiler flags are recorded only as SHA-256 of their exact CMakeCache.txt text (#1445):
        # none of that text, and none of its paths, appears anywhere in the manifest.
        self.assertEqual(manifest["build"]["cxx_flags_sha256"], hashlib.sha256(self.flags.encode("utf-8")).hexdigest())
        self.assertEqual(
            manifest["build"]["cxx_flags_config_sha256"], hashlib.sha256(self.config_flags.encode("utf-8")).hexdigest()
        )
        for probe in FLAG_PROBES:
            self.assertNotIn(probe.lower(), text.lower(), f"flag text {probe!r} is in the manifest")
        # The benchmark arguments: allowlisted options as written, a --benchmark_context value hashed.
        data_hash = hashlib.sha256(data_arg.split("=", 1)[1].encode("utf-8")).hexdigest()
        for expected in (
            "--benchmark_repetitions=10",
            "--benchmark_min_time=0.5s",
            "--benchmark_display_aggregates_only=true",
            "--benchmark_out_format=json",
            "--benchmark_filter=BM_X",
            f"--benchmark_out={results[0].name}",
            f"--benchmark_context=sha256:{data_hash}",
        ):
            self.assertIn(expected, manifest["benchmark"]["args"])
        self.assertTrue(manifest["benchmark"]["raw_repetitions"])
        self.assertFalse(manifest["benchmark"]["report_aggregates_only"])
        args = manifest["benchmark"]["args"]
        self.assertNotIn("--benchmark_report_aggregates_only=true", args)
        self.assertIn("--benchmark_filter=BM_X", args)
        self.assertIn(f"--benchmark_out={results[0].name}", args)
        self.assertEqual(manifest["machine"]["logical_cores"], os.cpu_count())

        # The user and host names as tokens, the home, temp and checkout paths as substrings.
        leaks = find_identity_leaks(
            manifest,
            tokens=[socket.gethostname(), getpass.getuser()],
            paths=[str(Path.home()), posix(Path.home()), tempfile.gettempdir(), str(REPO_ROOT), posix(REPO_ROOT), "/home/"],
        )
        self.assertEqual(leaks, [])
        self.assertNotRegex(text, r"host_?name|user_?name")


def load_bench_manifest():
    spec = importlib.util.spec_from_file_location("bench_manifest", REPO_ROOT / "tools" / "bench-manifest.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ScrubberTest(unittest.TestCase):
    """tools/bench-manifest.py's flag scrubber and identity pass, without bash."""

    def test_arguments_are_allowlisted_or_hashed(self):
        # #1445 review: allowlisted options with safe values as written, everything else hashed.
        module = load_bench_manifest()

        def digest(text: str) -> str:
            return hashlib.sha256(text.encode("utf-8")).hexdigest()

        verbatim = [
            "--benchmark_repetitions=10",
            "--benchmark_min_time=0.5s",
            "--benchmark_min_time=100x",
            "--benchmark_min_time=2",
            "--benchmark_min_warmup_time=0.25",
            "--benchmark_min_warmup_time=1.5e-1s",
            "--benchmark_display_aggregates_only=true",
            "--benchmark_report_aggregates_only=FALSE",
            "--benchmark_report_aggregates_only",
            "--benchmark_enable_random_interleaving=yes",
            "--benchmark_counters_tabular=1",
            "--benchmark_dry_run",
            "--benchmark_list_tests=t",
            "--benchmark_time_unit=ms",
            "--benchmark_format=csv",
            "--benchmark_out_format=json",
            "--benchmark_color=auto",
            "--v=2",
            "--benchmark_filter=BM_(A|B)$",
            "--benchmark_filter=",
        ]
        for argument in verbatim:
            with self.subTest(argument=argument):
                self.assertEqual(module.record_argument(argument), argument)
        hashed = [
            # A malformed allowlisted value.
            ("--benchmark_repetitions=10;rm", "--benchmark_repetitions", "10;rm"),
            ("--benchmark_min_time=/home/u/x", "--benchmark_min_time", "/home/u/x"),
            ("--benchmark_time_unit=hours", "--benchmark_time_unit", "hours"),
            ("--Benchmark_Repetitions=10", "--Benchmark_Repetitions", "10"),
            # --benchmark_context values with quoted, embedded and '=' paths (#1445 review).
            (
                '--benchmark_context=src="/srv/private-checkout/tasksmack/profiles/input.bin"',
                "--benchmark_context",
                'src="/srv/private-checkout/tasksmack/profiles/input.bin"',
            ),
            (
                "--benchmark_context=note=loaded /srv/private-checkout/tasksmack/profiles/input.bin",
                "--benchmark_context",
                "note=loaded /srv/private-checkout/tasksmack/profiles/input.bin",
            ),
            ("--benchmark_context=note=/srv/private=run/host/data.bin", "--benchmark_context", "note=/srv/private=run/host/data.bin"),
            ("--benchmark_perf_counters=CYCLES", "--benchmark_perf_counters", "CYCLES"),
            ("--some_unknown_flag=/home/u/x", "--some_unknown_flag", "/home/u/x"),
        ]
        for argument, name, value in hashed:
            with self.subTest(argument=argument):
                self.assertEqual(module.record_argument(argument), f"{name}=sha256:{digest(value)}")
                self.assertEqual(module.record_argument(argument), module.record_argument(argument))
        for whole in ("--some_unknown_switch", "/home/u/positional", "-x", "--benchmark_context"):
            with self.subTest(argument=whole):
                self.assertEqual(module.record_argument(whole), f"sha256:{digest(whole)}")
        self.assertEqual(module.record_argument("--benchmark_out=/tmp/x/out/fake-1.json"), "--benchmark_out=fake-1.json")
        self.assertNotEqual(module.record_argument("--benchmark_context=a=1"), module.record_argument("--benchmark_context=a=2"))

    def test_flag_hashes_are_what_the_benchmark_binary_was_linked_with(self):
        # #1445 review: reconfiguring CMAKE_CXX_FLAGS_<CONFIG> without a rebuild changes the cache,
        # not the binary; the build information beside the binary (copied when it links) carries
        # the hashes it was built with and is preferred. Older trees, or unusable build information,
        # fall back to the cache. The same cases as tools/test-bench.ps1.
        module = load_bench_manifest()
        linked, linked_config = hashlib.sha256(b"-O2 linked").hexdigest(), hashlib.sha256(b"-O3 linked").hexdigest()
        cache_text = "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS:STRING=-O2 reconfigured\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 reconfigured\n"
        from_cache = (hashlib.sha256(b"-O2 reconfigured").hexdigest(), hashlib.sha256(b"-O3 reconfigured").hexdigest())
        with tempfile.TemporaryDirectory() as tmp:
            for name, buildinfo, expected, source in (
                ("linked", {"ipo": "ON", "cxx_flags_sha256": linked, "cxx_flags_config_sha256": linked_config}, (linked, linked_config), "buildinfo"),
                ("linked-absent-entry", {"cxx_flags_sha256": None, "cxx_flags_config_sha256": linked_config}, (None, linked_config), "buildinfo"),
                ("no-buildinfo", None, from_cache, "cache"),
                ("ipo-only-buildinfo", {"ipo": "ON"}, from_cache, "cache"),
                ("malformed-hash", {"cxx_flags_sha256": "ABC", "cxx_flags_config_sha256": linked_config}, from_cache, "cache"),
            ):
                with self.subTest(case=name):
                    tree = Path(tmp) / name
                    (tree / "bin").mkdir(parents=True)
                    (tree / "CMakeCache.txt").write_text(cache_text, encoding="utf-8")
                    if buildinfo is not None:
                        (tree / "bin" / "TaskSmackBenchmarks.buildinfo.json").write_text(json.dumps(buildinfo), encoding="utf-8")
                    build = module.build_provenance(tree / "bin" / "TaskSmackBenchmarks")
                    self.assertEqual((build["cxx_flags_sha256"], build["cxx_flags_config_sha256"], build["cxx_flags_source"]), (*expected, source))

    def test_the_build_type_is_the_configuration_the_binary_was_linked_as(self):
        # #1445 review: a single-config tree reconfigured from Release to Debug without a rebuild
        # holds the Release binary and its build information while the cache says Debug. The same
        # cases as tools/test-bench.ps1.
        module = load_bench_manifest()
        with tempfile.TemporaryDirectory() as tmp:
            for name, cache, bin_dir, buildinfo, expected in (
                ("reconfigured", "CMAKE_BUILD_TYPE:STRING=Debug\n", "bin", {"config": "Release", "ipo": "ON"}, "Release"),
                ("no-cache", None, "bin", {"config": "Release"}, "Release"),
                ("multi-config", "", "bin/RelWithDebInfo", {"config": "RelWithDebInfo"}, "RelWithDebInfo"),
                ("no-buildinfo", "CMAKE_BUILD_TYPE:STRING=Debug\n", "bin", None, "Debug"),
                ("no-buildinfo-multi-config", "", "bin/Release", None, "Release"),
                ("empty-config", "CMAKE_BUILD_TYPE:STRING=Debug\n", "bin", {"config": "", "ipo": "ON"}, "Debug"),
                ("non-string-config", "CMAKE_BUILD_TYPE:STRING=Debug\n", "bin", {"config": 7}, "Debug"),
            ):
                with self.subTest(case=name):
                    tree = Path(tmp) / name
                    (tree / bin_dir).mkdir(parents=True)
                    if cache is not None:
                        (tree / "CMakeCache.txt").write_text(cache, encoding="utf-8")
                    if buildinfo is not None:
                        (tree / bin_dir / "TaskSmackBenchmarks.buildinfo.json").write_text(json.dumps(buildinfo), encoding="utf-8")
                    self.assertEqual(module.build_provenance(tree / bin_dir / "TaskSmackBenchmarks")["build_type"], expected)

    def test_ipo_is_what_the_benchmark_target_is_built_with(self):
        # #1445 review: CompilerOptions.cmake turns IPO on through a normal variable, so the cached
        # CMAKE_INTERPROCEDURAL_OPTIMIZATION can say OFF, and a multi-config generator can set IPO per
        # configuration. benchmarks/CMakeLists.txt writes each configuration's effective IPO to
        # TaskSmackBenchmarks.buildinfo.json next to its binary; it wins over the cache. Trees
        # without it keep the old fallbacks.
        module = load_bench_manifest()
        conflicting = "CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF\nTASKSMACK_ENABLE_IPO:BOOL=ON\n"
        with tempfile.TemporaryDirectory() as tmp:
            for name, cache, bin_dir, buildinfo, build_type, ipo, source in (
                ("single", conflicting, "bin", '{"config": "Release", "ipo": "ON"}', "Release", "ON", "buildinfo"),
                # Multi-config: no CMAKE_BUILD_TYPE; each bin/<Config>/ has its own build information,
                # each disagreeing with the generic cache value.
                ("multi-on", "CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=ON\n", "bin/Debug", '{"config": "Debug", "ipo": "OFF"}', "Debug", "OFF", "buildinfo"),
                ("multi-off", "CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF\n", "bin/Release", '{"config": "Release", "ipo": "ON"}', "Release", "ON", "buildinfo"),
                ("no-cache", None, "bin", '{"config": "Release", "ipo": "ON"}', "Release", "ON", "buildinfo"),
                # Unusable build information falls back to the cache.
                ("bad-value", conflicting, "bin", '{"ipo": "on"}', "Release", "OFF", "CMAKE_INTERPROCEDURAL_OPTIMIZATION"),
                ("bad-json", conflicting, "bin", '{"ipo": ', "Release", "OFF", "CMAKE_INTERPROCEDURAL_OPTIMIZATION"),
                ("old-cache", conflicting, "bin", None, "Release", "OFF", "CMAKE_INTERPROCEDURAL_OPTIMIZATION"),
                ("option-only", "TASKSMACK_ENABLE_IPO:BOOL=ON\n", "bin", None, "Release", "ON", "TASKSMACK_ENABLE_IPO"),
                ("unknown", "", "bin", None, "Release", None, None),
            ):
                with self.subTest(case=name):
                    tree = Path(tmp) / name
                    binary_dir = tree / bin_dir
                    binary_dir.mkdir(parents=True)
                    if cache is not None:
                        single = "CMAKE_BUILD_TYPE:STRING=Release\n" if bin_dir == "bin" else ""
                        (tree / "CMakeCache.txt").write_text(single + cache, encoding="utf-8")
                    if buildinfo is not None:
                        (binary_dir / "TaskSmackBenchmarks.buildinfo.json").write_text(buildinfo, encoding="utf-8")
                    build = module.build_provenance(binary_dir / "TaskSmackBenchmarks")
                    self.assertEqual((build["build_type"], build["ipo"], build["ipo_source"]), (build_type, ipo, source))

    def test_cache_entries_are_read_as_cmake_reads_them(self):
        # #1445 review: CMake writes KEY:TYPE=VALUE with any character but ':' in an unquoted key
        # (a custom build type's CMAKE_CXX_FLAGS_ASAN-UBSAN), quotes a key holding ':', and puts a
        # value with trailing whitespace in single quotes (cmState::ParseCacheEntry).
        module = load_bench_manifest()
        cache = module.parse_cmake_cache(
            "# This is the CMakeCache file.\n"
            "//Help text: not=an entry\n"
            "CMAKE_CXX_FLAGS_ASAN-UBSAN:STRING=-O2 -fsanitize=address\n"
            "CMAKE_CXX_FLAGS_REL.WITH+INFO:STRING=-O1\n"
            '"KEY:WITH=SPECIALS":STRING=colon\n'
            '"QUOTED_UNTYPED"=q\n'
            "UNTYPED=u\n"
            "  INDENTED:BOOL=ON\r\n"
            "TRAILING:STRING='-O3 '\n"
            "PADDED:STRING=-O2 \t\r\n"
            "LEADING:STRING= -O3\n"
            "EMPTY:STRING=\n"
            "lower_case:STRING=lower\n"
            "not an entry\n"
        )
        self.assertEqual(
            cache,
            {
                "CMAKE_CXX_FLAGS_ASAN-UBSAN": "-O2 -fsanitize=address",
                "CMAKE_CXX_FLAGS_REL.WITH+INFO": "-O1",
                "KEY:WITH=SPECIALS": "colon",
                "QUOTED_UNTYPED": "q",
                "UNTYPED": "u",
                "INDENTED": "ON",
                "TRAILING": "-O3 ",
                "PADDED": "-O2",
                "LEADING": " -O3",
                "EMPTY": "",
                "lower_case": "lower",
            },
        )
        self.assertEqual(module.cmake_upper("asan-ubsan.rel+info"), "ASAN-UBSAN.REL+INFO")
        # The configuration's flags are found and hashed for a hyphenated, dotted or plus-signed
        # custom build type.
        with tempfile.TemporaryDirectory() as tmp:
            for build_type, flags in (("ASan-UBSan", "-O2 -fsanitize=address"), ("Rel.With+Info", "-O1")):
                with self.subTest(build_type=build_type):
                    tree = Path(tmp) / build_type
                    (tree / "bin").mkdir(parents=True)
                    (tree / "CMakeCache.txt").write_text(
                        f"CMAKE_BUILD_TYPE:STRING={build_type}\nCMAKE_CXX_FLAGS_{build_type.upper()}:STRING={flags}\n",
                        encoding="utf-8",
                    )
                    build = module.build_provenance(tree / "bin" / "TaskSmackBenchmarks")
                    self.assertEqual(build["build_type"], build_type)
                    self.assertEqual(build["cxx_flags_config_sha256"], hashlib.sha256(flags.encode("utf-8")).hexdigest())

    def test_a_user_or_host_name_in_the_preset_is_hidden_between_preset_separators(self):
        # #1445 review: the preset's own '-', '.' and '_' bound a user or host name in it too.
        module = load_bench_manifest()
        hosts = ["bench-host-123", "bench-host-123.example.com"]
        for preset, expected in (
            ("benchuser", "<user>"),
            ("BENCHUSER", "<user>"),
            ("win-benchuser", "win-<user>"),
            ("benchuser.release_x", "<user>.release_x"),
            ("bench-host-123", "<host>"),
            ("ci-bench-host-123.example.com-nightly", "ci-<host>-nightly"),
            # Not the name on its own: left alone.
            ("benchusers", "benchusers"),
            ("x86_64-RelWithDebInfo", "x86_64-RelWithDebInfo"),
            ("win-benchmark", "win-benchmark"),
        ):
            with self.subTest(preset=preset):
                self.assertEqual(module.hide_name_identity(preset, "benchuser", hosts), expected)
        # A user name under 3 characters is never replaced on its own.
        self.assertEqual(module.hide_name_identity("ab-release", "ab", []), "ab-release")
        self.assertEqual(module.preset_file_stem("win-<user>-<host>"), "win-user-host")

    @unittest.skipUnless(shutil.which("git"), "git not available")
    def test_a_user_or_host_name_in_the_branch_is_hidden(self):
        # #1445 review: a user or host name in the branch is hidden between the branch's own '-',
        # '.', '_' and '/' too. The same cases as tools/test-bench.ps1.
        module = load_bench_manifest()
        hosts = ["bench-host-123", "bench-host-123.example.com"]
        with tempfile.TemporaryDirectory() as tmp:
            git = ["git", "-C", tmp, "-c", "user.name=bench-test", "-c", "user.email=bench-test@example.invalid"]
            subprocess.run([*git, "init", "-q"], check=True, capture_output=True)
            subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "init"], check=True, capture_output=True)
            for branch, expected in (
                ("benchuser-fix", "<user>-fix"),
                ("feature/benchuser", "feature/<user>"),
                ("BenchUser_wip", "<user>_wip"),
                ("bench-host-123.example.com-test", "<host>-test"),
                ("ci/bench-host-123/nightly", "ci/<host>/nightly"),
                ("benchusers-x", "benchusers-x"),
            ):
                with self.subTest(branch=branch):
                    subprocess.run([*git, "checkout", "-q", "-b", branch], check=True, capture_output=True)
                    self.assertEqual(module.git_provenance(Path(tmp), "benchuser", hosts)["branch"], expected)

    def test_an_absent_cache_entry_hashes_as_null_an_empty_one_as_empty(self):
        # #1445 review: unknown flags stay distinguishable from explicitly empty ones.
        module = load_bench_manifest()
        self.assertIsNone(module.text_sha256(None))
        empty = hashlib.sha256(b"").hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            for name, lines, expected in (
                ("absent", "CMAKE_BUILD_TYPE:STRING=Release\n", None),
                ("empty", "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS:STRING=\nCMAKE_CXX_FLAGS_RELEASE:STRING=\n", empty),
            ):
                with self.subTest(case=name):
                    tree = Path(tmp) / name
                    (tree / "bin").mkdir(parents=True)
                    (tree / "CMakeCache.txt").write_text(lines, encoding="utf-8")
                    build = module.build_provenance(tree / "bin" / "TaskSmackBenchmarks")
                    self.assertEqual((build["cxx_flags_sha256"], build["cxx_flags_config_sha256"]), (expected, expected))

    def test_flags_hash_equally_and_change_with_a_flag(self):
        # #1445: equal flags hash equally across build trees; a changed flag changes the hash.
        module = load_bench_manifest()
        flags, config_flags = raw_flags("exampleuser", "/home/exampleuser")
        with tempfile.TemporaryDirectory() as tmp:
            hashes = {}
            for name, value in (("one", flags), ("same", flags), ("changed", flags + " -fno-rtti")):
                tree = Path(tmp) / name
                write_build_tree(tree, "4.1.0", {}, value, config_flags)
                hashes[name] = module.build_provenance(tree / "bin" / "TaskSmackBenchmarks")
        self.assertEqual(hashes["one"]["cxx_flags_sha256"], hashlib.sha256(flags.encode("utf-8")).hexdigest())
        self.assertEqual(hashes["one"]["cxx_flags_sha256"], hashes["same"]["cxx_flags_sha256"])
        self.assertNotEqual(hashes["one"]["cxx_flags_sha256"], hashes["changed"]["cxx_flags_sha256"])
        self.assertEqual(hashes["one"]["cxx_flags_config_sha256"], hashes["changed"]["cxx_flags_config_sha256"])
        self.assertIsNone(module.text_sha256(None))

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
        # A short home directory is still a home prefix (#1445 review); only a root or a bare drive
        # is never one. The same cases as tools/test-bench.ps1.
        prefixes = module.home_prefixes(["/ab/", "C:\\ab"])
        self.assertEqual(
            module.hide_identity(["--benchmark_filter=/ab/data", "-DX=ab", "C:\\ab\\x"], prefixes, "ab"),
            ["--benchmark_filter=<home>/data", "-DX=ab", "<home>\\x"],
        )
        self.assertEqual(module.home_prefixes(["/", "\\", "//", "C:\\", "D:", "", None]), [])

    def test_identity_pass_leaves_validated_categorical_fields_alone(self):
        # #1445 review: host and user names that coincide with OS and compiler values change only
        # the free-form fields.
        module = load_bench_manifest()
        sample = {
            "schema_version": 1,
            "generator": "tools/bench.sh",
            "preset": "Linux-preset",
            "git": {"commit": "a" * 40, "branch": "clang/Linux", "dirty": False},
            "build": {
                "build_type": "Release",
                "generator": "Ninja",
                "compiler": "clang",
                "compiler_id": "Clang",
                "compiler_version": "22.1.8",
                "cxx_flags_sha256": "a" * 64,
            },
            "benchmark": {"args": ["--benchmark_context=os=Windows", "-DHOST=Linux -DBY=clang -DCC=GNU"], "raw_repetitions": True},
            "machine": {
                "label": "x",
                "cpu_model": "Linux Box CPU",
                "logical_cores": 8,
                "os_name": "Linux",
                "os_version": "6.1",
                "arch": "x86_64",
            },
        }
        for os_host in ("Linux", "Windows"):
            with self.subTest(host=os_host):
                hidden = module.hide_manifest_identity(sample, [], "clang", [os_host, "GNU"])
                linux = "<host>" if os_host == "Linux" else "Linux"
                windows = "<host>" if os_host == "Windows" else "Windows"
                self.assertEqual(
                    {k: hidden["machine"][k] for k in ("os_name", "os_version", "arch", "logical_cores")},
                    {"os_name": "Linux", "os_version": "6.1", "arch": "x86_64", "logical_cores": 8},
                )
                for key in ("build_type", "generator", "compiler_id", "compiler_version"):
                    self.assertEqual(hidden["build"][key], sample["build"][key], key)
                self.assertEqual(hidden["git"]["commit"], "a" * 40)
                self.assertIs(hidden["git"]["dirty"], False)
                self.assertEqual(hidden["schema_version"], 1)
                self.assertIs(hidden["benchmark"]["raw_repetitions"], True)
                # Free-form fields are still scrubbed.
                self.assertEqual(hidden["build"]["compiler"], "<user>")
                self.assertEqual(hidden["git"]["branch"], f"<user>/{linux}")
                self.assertEqual(hidden["build"]["cxx_flags_sha256"], "a" * 64)
                self.assertEqual(
                    hidden["benchmark"]["args"], [f"--benchmark_context=os={windows}", f"-DHOST={linux} -DBY=<user> -DCC=<host>"]
                )
                cpu = f"{linux} Box CPU"
                self.assertEqual(hidden["machine"]["cpu_model"], cpu)
                self.assertEqual(hidden["machine"]["label"], f"{cpu} / 8 logical cores / Linux 6.1")

    def test_a_name_in_a_custom_kernel_release_is_hidden(self):
        # #1445 review: a Linux kernel built with CONFIG_LOCALVERSION reports its suffix in
        # platform.release(); the OS version is otherwise exempt. The same cases as
        # tools/test-bench.ps1.
        module = load_bench_manifest()
        for os_name, os_version, user, hosts, expected in (
            ("Linux", "6.8.0-benchhost", "someone", ["benchhost"], "6.8.0-<host>"),
            ("Linux", "6.8.0-45-generic", "someone", ["benchhost"], "6.8.0-45-generic"),
            ("Linux", "6.8.0-benchuser_rt", "benchuser", [], "6.8.0-<user>_rt"),
            ("Linux", "6.8.0-benchhosts", "someone", ["benchhost"], "6.8.0-benchhosts"),
            # Windows reports a build number; the same treatment applies, for parity.
            ("Windows", "10.0.26300.0", "someone", ["benchhost"], "10.0.26300.0"),
            ("Windows", "10.0.26300-benchhost", "someone", ["benchhost"], "10.0.26300-<host>"),
        ):
            with self.subTest(os_version=os_version):
                machine = {"label": "x", "cpu_model": "Some CPU", "logical_cores": 8, "os_name": os_name, "os_version": os_version, "arch": "x86_64"}
                hidden = module.hide_manifest_identity({"machine": machine}, [], user, hosts)["machine"]
                self.assertEqual(hidden["os_version"], expected)
                self.assertEqual(hidden["label"], f"Some CPU / 8 logical cores / {os_name} {expected}")

    def test_custom_build_types_are_scrubbed_standard_ones_kept(self):
        # #1445 review: only CMake's standard configurations are exempt as build types.
        module = load_bench_manifest()
        for build_type, user, hosts, expected in (
            ("benchuser", "benchuser", [], "<user>"),
            ("benchhost", "someone", ["benchhost"], "<host>"),
            ("Release", "Release", ["Release"], "Release"),
            ("RelWithDebInfo", "RelWithDebInfo", [], "RelWithDebInfo"),
            # A compound custom build type: its own '-', '.' and '_' bound the name too.
            ("ASan-benchuser", "benchuser", [], "ASan-<user>"),
            ("Release_benchhost", "someone", ["benchhost"], "Release_<host>"),
            ("ci.BenchUser", "benchuser", [], "ci.<user>"),
            ("ASan-benchusers", "benchuser", [], "ASan-benchusers"),
            ("ASan-UBSan", "asan", [], "<user>-UBSan"),
        ):
            with self.subTest(build_type=build_type):
                hidden = module.hide_manifest_identity({"build": {"build_type": build_type}}, [], user, hosts)
                self.assertEqual(hidden["build"]["build_type"], expected)

    def test_identity_pass_hides_injected_host_names(self):
        # #1445 review: the host name (short and FQDN) is hidden like the user name.
        module = load_bench_manifest()
        hosts = ["bench-host-123", "bench-host-123.example.com"]
        cases = [
            ("--benchmark_context=tsk_ctx_machine=bench-host-123", "--benchmark_context=tsk_ctx_machine=<host>"),
            ("--benchmark_context=tsk_ctx_machine=BENCH-HOST-123.example.com", "--benchmark_context=tsk_ctx_machine=<host>"),
            ("ssh://bench-host-123.example.com/x", "ssh://<host>/x"),
            ("xbench-host-123y -DHOST_bench-host-123", "xbench-host-123y -DHOST_bench-host-123"),
        ]
        for given, expected in cases:
            with self.subTest(given=given):
                self.assertEqual(module.hide_identity(given, [], "someone", hosts), expected)
        self.assertEqual(module.hide_identity("tsk_ctx_machine=ab", [], "someone", ["ab"]), "tsk_ctx_machine=ab")
        self.assertIn(socket.gethostname(), module.host_names())


class IdentityLeakCheckTest(unittest.TestCase):
    """The leak check used on the manifest, with a controlled user and home (#1445 review)."""

    CLEAN = {
        "build": {"build_type": "Release", "compiler": "clang++"},
        "benchmark": {"args": ["--sysroot=<abs>/sysroot", "-DBUILD=1", "-DCMAKE_BUILD=on", "--benchmark_filter=BM_Build"]},
    }

    def test_a_clean_manifest_passes_for_a_flag_word_user_name(self):
        # root: --sysroot= keeps "root" inside a word; build: the "build" key and -DBUILD=1.
        for user in ("root", "build"):
            with self.subTest(user=user):
                self.assertEqual(find_identity_leaks(self.CLEAN, [user], [f"/home/{user}", f"C:\\Users\\{user}"]), [])

    def test_a_real_leak_is_still_found(self):
        for user in ("root", "build"):
            for leaky in (f"-DBUILT_BY={user}", f"E:/Users/{user}/x", f"/home/{user}/src"):
                with self.subTest(user=user, leaky=leaky):
                    dirty = {"benchmark": {"args": [f"-O2 {leaky}"]}}
                    self.assertNotEqual(find_identity_leaks(dirty, [user], [f"/home/{user}"]), [])


    def test_the_checker_skips_exactly_the_exempt_fields(self):
        # #1445 review: a host named after the OS or a user named after a standard build type is no
        # leak in the fields the writer exempts; the free-form fields are still checked, and the
        # label must be the one built from the machine fields.
        module = load_bench_manifest()
        for os_name, version, token in (("Linux", "6.1", "Linux"), ("Windows", "10.0.26100", "Windows"), ("Linux", "6.1", "Release")):
            with self.subTest(token=token):
                machine = {"cpu_model": "Some CPU", "logical_cores": 8, "os_name": os_name, "os_version": version, "arch": "x86_64"}
                machine["label"] = module.machine_label(machine)
                manifest = {"build": {"build_type": "Release", "generator": "Ninja"}, "machine": machine}
                self.assertEqual(find_identity_leaks(manifest, [token], []), [])
                machine["cpu_model"] = f"{token} Box"
                self.assertNotEqual(find_identity_leaks(manifest, [token], []), [])
        tampered = {"machine": {"label": "benchhost / 8 logical cores / Linux 6.1", "cpu_model": "Some CPU", "logical_cores": 8, "os_name": "Linux", "os_version": "6.1"}}
        self.assertNotEqual(find_identity_leaks(tampered, ["benchhost"], []), [])
        self.assertNotEqual(find_identity_leaks({"build": {"build_type": "benchuser"}}, ["benchuser"], []), [])

    def test_the_checker_covers_an_injected_host_name(self):
        hosts = ["bench-host-123", "bench-host-123.example.com"]
        clean = {"benchmark": {"args": ["--benchmark_context=tsk_ctx_machine=<host>", "--benchmark_filter=BM_bench-host-123x"]}}
        self.assertEqual(find_identity_leaks(clean, hosts, []), [])
        for leaky in ("--benchmark_context=tsk_ctx_machine=bench-host-123", "ssh://bench-host-123.example.com/x"):
            with self.subTest(leaky=leaky):
                self.assertNotEqual(find_identity_leaks({"benchmark": {"args": [leaky]}}, hosts, []), [])


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
