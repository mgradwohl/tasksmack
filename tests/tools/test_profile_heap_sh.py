#!/usr/bin/env python3
"""Tests for tools/profile-heap.sh's benchmark selection (#2987) and app mode's --duration (#3406).

Google Benchmark exits 0 when --benchmark_filter matches nothing, and heaptrack then records only its startup
and registration as though it were the requested workload. The script runs here from a copy in a scratch
checkout against a stub benchmark binary and a stub heaptrack that records being started, so nothing is
profiled. Needs bash; Linux only, as the script is.
"""

import os
import shutil
import signal
import subprocess
import tempfile
import textwrap
import time
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

# As heaptrack does: leaves its output file, runs the program as its own child (so the script must signal the
# program, not this wrapper) and exits with the program's code.
STUB_HEAPTRACK_RUNNING_THE_APP = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    [[ "$1" == -o ]] && : > "$2.1234.gz"
    shift 2
    sleep "${STUB_HEAPTRACK_DELAY:-0}"   # a slow start: TaskSmack isn't there yet
    "$@"
    exit $?
    """
)
# A TaskSmack stand-in, by STUB_APP_MODE: "clean" logs the main loop and exits 0 on SIGTERM; "early" logs it and
# exits on its own after a second; "blink" exits 0.3 s after logging it (inside --duration's last second);
# "silent" never logs it and exits; "dirty" exits 3 on SIGTERM; "stubborn" ignores SIGTERM.
STUB_APP = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    mode="${STUB_APP_MODE:-clean}"
    [[ "$mode" == silent ]] && exit 0
    echo "Entering main loop"
    case "$mode" in
        dirty) trap 'exit 3' TERM ;;
        stubborn) trap '' TERM ;;
        *) trap 'echo closed >> "$STUB_APP_EVENTS"; exit 0' TERM ;;
    esac
    if [[ "$mode" == early ]]; then sleep 1; exit 0; fi
    if [[ "$mode" == blink ]]; then sleep 0.3; exit 0; fi
    while :; do sleep 0.1; done
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



class ProfileHeapDurationTest(unittest.TestCase):
    """App mode's --duration (#3406): an unattended capture closes TaskSmack itself and finishes."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="profile_heap_duration_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "tools").mkdir()
        for name in ("profile-heap.sh", "common.sh"):
            shutil.copy2(REPO_ROOT / "tools" / name, self.root / "tools" / name)
        app = self.root / "build" / "profile" / "bin" / "TaskSmack"
        app.parent.mkdir(parents=True)
        app.write_text(STUB_APP, encoding="utf-8")
        app.chmod(0o755)
        bin_dir = self.root / "stub-bin"
        bin_dir.mkdir()
        (bin_dir / "heaptrack").write_text(STUB_HEAPTRACK_RUNNING_THE_APP, encoding="utf-8")
        (bin_dir / "heaptrack").chmod(0o755)
        self.events = self.root / "app-events.txt"
        self.env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "HOME": str(self.root),
            "STUB_APP_EVENTS": str(self.events),
        }

    def profile(self, mode, *extra):
        return subprocess.run(
            [BASH, str(self.root / "tools" / "profile-heap.sh"), "app", "--preset", "profile", "--skip-build", *extra],
            capture_output=True,
            text=True,
            env={**self.env, "STUB_APP_MODE": mode},
            cwd=self.root,
            check=False,
            timeout=60,
        )

    def test_the_app_is_closed_after_the_duration_and_the_capture_completes(self):
        result = self.profile("clean", "--duration", "1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Capture complete", result.stdout)
        # The signal reached TaskSmack itself, not just heaptrack's wrapper.
        self.assertEqual(self.events.read_text(encoding="utf-8").strip(), "closed")
        log = next((self.root / "perf-data").glob("heaptrack-app-*[0-9].log")).read_text(encoding="utf-8")
        self.assertIn("DURATION_SECONDS=1", log)
        self.assertIn("END_REASON=closed cleanly", log)

    def test_an_app_that_exits_before_the_duration_fails_the_run(self):
        result = self.profile("early", "--duration", "5")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("exited on its own before --duration elapsed", result.stderr)

    def test_an_exit_during_the_last_second_still_fails_the_run(self):
        # --duration 1 is one second: an app gone 0.3 s in must not pass as a whole capture.
        result = self.profile("blink", "--duration", "1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("exited on its own before --duration elapsed", result.stderr)

    def test_an_app_that_ignores_sigterm_is_killed_and_fails_the_run(self):
        result = self.profile("stubborn", "--duration", "1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("did not exit within 10s of the script's SIGTERM and was killed", result.stderr)

    def test_ctrl_c_before_the_app_starts_leaves_nothing_running(self):
        # heaptrack takes 2 s to start TaskSmack; Ctrl+C comes after 0.5 s, before the script can find it.
        process = subprocess.Popen(
            [BASH, str(self.root / "tools" / "profile-heap.sh"), "app", "--preset", "profile", "--skip-build",
             "--duration", "5"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**self.env, "STUB_APP_MODE": "clean", "STUB_HEAPTRACK_DELAY": "2"}, cwd=self.root,
        )
        time.sleep(0.5)
        process.send_signal(signal.SIGINT)
        _, stderr = process.communicate(timeout=60)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("interrupted", stderr)
        time.sleep(3)  # past the stub's delayed launch: a TaskSmack started late would be running by now
        app = str(self.root / "build" / "profile" / "bin" / "TaskSmack")
        left = subprocess.run(["pgrep", "-f", app], capture_output=True, text=True, check=False).stdout.split()
        for pid in left:
            os.kill(int(pid), signal.SIGKILL)
        self.assertEqual(left, [], "a TaskSmack outlived the interrupted run")

    def test_an_app_that_never_reaches_its_main_loop_fails_the_run(self):
        result = self.profile("silent", "--duration", "1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Entering main loop", result.stderr)

    def test_a_non_zero_exit_after_the_signal_fails_the_run(self):
        result = self.profile("dirty", "--duration", "1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("exited with code 3 after the script's SIGTERM", result.stderr)

    def test_duration_is_app_mode_only_and_a_whole_number(self):
        bench = subprocess.run(
            [BASH, str(self.root / "tools" / "profile-heap.sh"), "bench", "--skip-build", "--duration", "5"],
            capture_output=True, text=True, env=self.env, cwd=self.root, check=False,
        )
        self.assertNotEqual(bench.returncode, 0)
        self.assertIn("--duration applies to app mode only", bench.stderr)
        self.assertIn("whole number", self.profile("clean", "--duration", "1.5").stderr)


if __name__ == "__main__":
    unittest.main()
