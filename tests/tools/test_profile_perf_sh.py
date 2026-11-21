#!/usr/bin/env python3
"""Tests for tools/profile-perf.sh (#4211, #4212, #4222, #4223, #4210, #4225): bench mode and the option checks,
the perf event probe, and app mode's lifecycle, against a stub perf, a stub TaskSmackBenchmarks and a stand-in
TaskSmack, so nothing is profiled or built. Needs bash; Linux only, as the script is."""

from __future__ import annotations

import os
import shutil
import signal
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

# perf: counts only cpu-clock (as WSL2), or cycles too with STUB_PERF_CYCLES; records its record arguments, writes
# -o's file, and exits $STUB_PERF_RECORD_EXIT from record. Attached with -p (app mode), record runs the command after
# `--` (`sleep N` for --duration), or else waits for the process to exit, as perf record -p does.
# `report --stats` says $STUB_PERF_SAMPLES (unless STUB_PERF_NO_STATS), and `script -G` prints that many lines.
STUB_PERF = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    case "$1" in
        --version) echo "perf version 6.0.stub" ;;
        stat)
            [[ "$*" == *cpu-clock* ]] && echo "1.00,msec,cpu-clock,1000,100.00,,"
            [[ "$*" == *cycles* && -n "${STUB_PERF_CYCLES:-}" ]] && echo "123456,,cycles,1000,100.00,,"
            exit 0 ;;
        record)
            shift
            printf '%s\\n' "$@" > "$STUB_PERF_CALLS"
            out=""
            pid=""
            command=()
            while [[ $# -gt 0 ]]; do
                case "$1" in
                    -o) out="$2"; shift ;;
                    -p) pid="$2"; shift ;;
                    --) shift; command=("$@"); break ;;
                esac
                shift
            done
            [[ -n "$out" ]] && : > "$out"
            # As perf does, Ctrl+C ends the recording, and the data is written: not a failure.
            trap 'exit "${STUB_PERF_RECORD_EXIT:-0}"' INT
            if [[ -n "$pid" ]]; then
                if [[ ${#command[@]} -gt 0 ]]; then
                    "${command[@]}"
                else
                    while kill -0 "$pid" 2>/dev/null; do sleep 0.1; done
                fi
            fi
            exit "${STUB_PERF_RECORD_EXIT:-0}" ;;
        script)
            for ((i = 0; i < ${STUB_PERF_SAMPLES:-3}; i++)); do echo "sample $i"; done ;;
        report)
            # --stats: perf's "SAMPLE events:" line, unless STUB_PERF_NO_STATS (an older perf's output).
            [[ -z "${STUB_PERF_NO_STATS:-}" ]] && echo "           SAMPLE events:  ${STUB_PERF_SAMPLES:-3}"
            exit 0 ;;
    esac
    exit 0
    """
)


class ProfilePerfCase(unittest.TestCase):
    """The copied script, a stub perf and the build folders both test classes use; it has no tests of its own."""

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


@unittest.skipUnless(sys.platform.startswith("linux"), "profile-perf.sh is Linux-only")
class ProfilePerfBenchTest(ProfilePerfCase):

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

    def test_the_sample_count_comes_from_report_stats_with_a_script_fallback(self):
        # #4224: perf script over a large trace is slow; perf report --stats reads the headers.
        result = self.bench(env={"STUB_PERF_SAMPLES": "42"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Captured 42 samples.", result.stdout + result.stderr)
        result = self.bench(env={"STUB_PERF_SAMPLES": "7", "STUB_PERF_NO_STATS": "1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Captured 7 samples.", result.stdout + result.stderr)
        result = self.bench(env={"STUB_PERF_SAMPLES": "0"})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("zero samples", result.stderr)

    def write_compile_db(self, entries):
        import json
        (self.root / "build" / "profile" / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def test_the_compiled_flags_come_from_the_profiled_binary_or_say_they_dont(self):
        # #4215: bench mode used to take the first src/ file of any target (the helper's, say) as "what was profiled".
        root = str(self.root)
        helper = {"file": f"{root}/src/Helper/main.cpp", "output": "src/CMakeFiles/TaskSmackHelper.dir/main.cpp.o",
                  "arguments": ["clang++", "-O2", "-DHELPER", "-c", f"{root}/src/Helper/main.cpp"]}
        bench = {"file": f"{root}/benchmarks/bench_ProcessModel.cpp",
                 "output": "benchmarks/CMakeFiles/TaskSmackBenchmarks.dir/bench_ProcessModel.cpp.o",
                 "arguments": ["clang++", "-O2", "-g", "-DBENCH", "-c", f"{root}/benchmarks/bench_ProcessModel.cpp"]}
        self.write_compile_db([helper, bench])
        result = self.bench()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("-DBENCH", result.stdout + result.stderr)
        log = next((self.root / "perf-data").glob("perf-bench-*[0-9].log")).read_text(encoding="utf-8")
        self.assertIn("COMPILE_SOURCE=own", log)
        self.write_compile_db([helper])
        result = self.bench()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("a sample from another target", result.stdout + result.stderr)

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



# A stand-in TaskSmack for app mode, as $STUB_APP says: "normal" logs the main-loop marker and quits cleanly on
# SIGTERM; "hang" ignores SIGTERM (the script then needs SIGKILL); "crash" exits 134 on it; "already" is turned away
# by the single-instance lock (code 75); "early" reaches its main loop and quits on its own at once.
STUB_APP = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    case "${STUB_APP:-normal}" in
        already) echo "TaskSmack is already running"; exit 75 ;;
        early) echo "Entering main loop"; sleep 0.3; exit 0 ;;
    esac
    on_term() {
        case "${STUB_APP:-normal}" in
            hang) return ;;
            crash) exit 134 ;;
        esac
        exit 0
    }
    trap on_term TERM
    echo "Entering main loop"
    while :; do sleep 0.1; done
    """
)


@unittest.skipUnless(sys.platform.startswith("linux"), "profile-perf.sh is Linux-only")
class ProfilePerfAppTest(ProfilePerfCase):
    """#4225: the perf event probe, and app mode: main loop, warm-up, the --duration close (SIGTERM, then SIGKILL
    after 10 s), Ctrl+C, an exit before --duration, and the already-running code 75. The stub perf and folders, plus a
    stand-in TaskSmack."""

    def setUp(self):
        super().setUp()
        app = self.root / "build" / "profile" / "bin" / "TaskSmack"
        app.write_text(STUB_APP, encoding="utf-8")
        app.chmod(0o755)

    def app(self, *args, env=None):
        return self.run_perf("app", "--skip-build", *args, env=env)

    def log(self):
        return next((self.root / "perf-data").glob("perf-app-*[0-9].log")).read_text(encoding="utf-8")

    def test_without_counting_cycles_it_falls_back_to_cpu_clock_and_says_so(self):
        result = self.bench()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("hardware 'cycles' event unavailable", result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(calls[calls.index("-e") + 1], "cpu-clock")
        result = self.bench(env={"STUB_PERF_CYCLES": "1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("cycles' event unavailable", result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(calls[calls.index("-e") + 1], "cycles")

    def test_a_duration_run_attaches_after_the_main_loop_and_closes_taskSmack_cleanly(self):
        result = self.app("--warmup", "0", "--duration", "1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(calls[calls.index("--") + 1 :], ["sleep", "1"])
        self.assertIn("-p", calls)
        log = self.log()
        self.assertIn("MAIN_LOOP_SEEN=1", log)
        self.assertIn("APP_EXIT_CODE=0", log)
        self.assertIn("APP_END_REASON=closed cleanly by the script's SIGTERM", log)

    def test_a_leading_zero_is_decimal_not_octal(self):
        # "08" isn't octal: Bash's -gt and $(( )) would abort on it without the 10# prefix.
        result = self.app("--warmup", "08", "--duration", "09", env={"STUB_APP": "already"})
        self.assertNotIn("value too great for base", result.stderr)
        self.assertIn("already running", result.stderr)

    def test_the_already_running_exit_is_a_failure_with_its_reason(self):
        result = self.app("--warmup", "0", "--duration", "1", env={"STUB_APP": "already"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("another TaskSmack is already running", result.stderr)
        self.assertFalse(self.calls.exists(), "perf never attached")

    def test_an_exit_before_the_duration_fails_the_run(self):
        result = self.app("--warmup", "0", "--duration", "5", env={"STUB_APP": "early"})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("exited", result.stderr)

    def test_a_crash_on_close_fails_the_run(self):
        result = self.app("--warmup", "0", "--duration", "1", env={"STUB_APP": "crash"})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("exited with a non-zero code after the script's SIGTERM", result.stderr)
        self.assertIn("APP_EXIT_CODE=134", self.log())

    def test_a_taskSmack_that_ignores_sigterm_is_killed_after_ten_seconds_and_fails(self):
        result = self.app("--warmup", "0", "--duration", "1", env={"STUB_APP": "hang"})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("did not exit within 10s of SIGTERM and was killed with SIGKILL", result.stderr)

    def test_ctrl_c_stops_the_capture_and_closes_taskSmack(self):
        process = subprocess.Popen(
            [BASH, str(self.root / "tools" / "profile-perf.sh"), "app", "--skip-build", "--warmup", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=self.env, cwd=self.root,
            start_new_session=True,
        )
        self.addCleanup(lambda: process.poll() is None and os.killpg(process.pid, signal.SIGKILL))
        deadline = 30
        while deadline > 0 and not self.calls.exists():
            try:
                process.wait(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                deadline -= 0.2
        self.assertTrue(self.calls.exists(), "perf attached")
        os.killpg(process.pid, signal.SIGINT)  # Ctrl+C reaches the whole foreground group
        stdout, stderr = process.communicate(timeout=60)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertIn("APP_END_REASON=closed cleanly by the script's SIGTERM", self.log())


if __name__ == "__main__":
    unittest.main()
