#!/usr/bin/env python3
"""Runs the shell of workflow steps whose bugs lived only inside a `run:` block (#2478).

- heavy-checks.yml benchmark-regression "Check for regression" and "Re-measure flagged benchmarks across
  launches": a flagged benchmark runs in 4 more launches (a stub bench.sh), and the median across the 5 is
  checked at the same thresholds, so one slow launch passes and a real regression stays red (#3476). Flags
  that stand go to benchmark-second-runner ("Hand the standing flags to a second runner"), whose "Re-measure
  the flagged benchmarks on this runner" decides the gate from 5 launches of its own, so one slow host
  can't fail it alone and a real regression still does (#3623).
- renovate.yml "Select dry-run mode": a scheduled or `disabled` run must not set RENOVATE_DRY_RUN,
  which Renovate reads as a dry run whatever its value (#2527).
- sanitizers.yml "Add sanitizer summary to workflow summary": the report's file name is printed in
  Markdown code marks, not run as a command substitution (#2532).
- main-health.yml "Open, update or close the tracking issue": a job only some runs execute (CI's
  weekly unity build) stays red until a run that ran it passes (#2313). `gh` is a stub that answers
  from fixtures through the step's own jq filters; only the step's `judge_branch` function runs,
  cut before the issue handling that follows it, for main (#2741 tracks release branches the same way).
- release.yml verify-reproducibility-windows "Compare digests": matching packages pass whatever the
  build-inputs manifests say, and only differing packages fail (#2952); an NSIS installer that differs
  fails too, since it isn't a .zip to open (#2302). Run with pwsh the way GitHub's `shell: pwsh` wrapper
  runs it, against fixture .zips, installers and manifests and the real comparator script.
- release.yml create-release "Verify the package set" and build-windows "Stage the installer's first
  pass": the NSIS installers are part of the exact release set, and the installer's first pass is handed
  on with exactly one of each file (#2302).
- release.yml create-release "Write SHA256SUMS" and "Verify the release's file set": one `sha256sum`-format
  SHA256SUMS lists every release file, and the signed set holds it and SHA256SUMS.bundle but no per-file
  Sigstore bundle (#3145); verify-published-release "Verify SHA256SUMS and every asset against it" pins
  cosign to the run's own ref and fails a download that doesn't match SHA256SUMS (`cosign` is a stub).
- codeql.yml "Keep results under src/": a result is kept when any of its locations, code-flow steps
  or related locations is under src/, so a taint flow from src/ into a third-party sink survives;
  only results with no src/ location at all are dropped (#2992).
- release.yml sign-windows and sign-windows-installer "Register the PowerShell Gallery if the runner has
  none": runs just before the signing action, whose Install-Module needs PSGallery, and registers the
  default gallery only when it's missing (#3109). Run with pwsh against stub repository cmdlets.
- changelog.yml release-notes "Wait for the release to be published" and "Replace the placeholder
  notes": the job waits out a missing or draft release and fails once its limit passes, then replaces
  release.yml's placeholder body with tools/compose-release-notes.py's notes and never edits notes
  someone wrote (#3144). `gh` is a stub; the composer and the guide's template are the real ones.
- .github/actions/setup-llvm "LLVM package cache key" and install-llvm.sh: every caller gets a weekly LLVM
  package cache key with a restore-key, a pinned release keeps its exact key (#3393), and the script
  installs an exact hit without apt.llvm.org, refreshes an older entry and falls back to it when
  apt.llvm.org is down, and still fails a cold miss (#3635). sudo, apt-get, curl and dpkg are stubs.

Each step is read from the workflow file itself (tests/tools/workflow_steps.py), its `${{ }}`
expressions replaced by fixed values, and run with bash (pwsh for the pwsh step). Needs bash; the
Main Health and CodeQL cases also need jq, and the reproducibility cases pwsh, and are skipped
without them.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from workflow_steps import find_step, step_names, substitute  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"
PWSH = shutil.which("pwsh")


def posix(path: Path) -> str:
    """A path bash accepts on every platform (forward slashes; Git Bash maps C:/ itself)."""
    return str(path).replace("\\", "/")


def run_bash(script: str, directory: Path, env: dict[str, str]) -> subprocess.CompletedProcess:
    path = directory / "step.sh"
    path.write_text(script + "\n", encoding="utf-8", newline="\n")
    return subprocess.run(
        [BASH, posix(path)],
        cwd=directory,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
        env={**os.environ, **env},
        timeout=60,
    )


class HeavyChecksBenchmarkBuildTests(unittest.TestCase):
    """#3052: benchmark-regression builds only the binary it runs, not the preset's whole tree."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"
    JOB = "benchmark-regression"

    def test_the_build_is_the_benchmark_target_alone(self):
        build = find_step(self.WORKFLOW, "Build", job=self.JOB)
        # The default target also built TaskSmack, the UI training driver and every test suite, which
        # pushed the job past its timeout on a cold or diverged ccache.
        self.assertEqual(build.run, "cmake --build --preset benchmark --target TaskSmackBenchmarks")

    def test_the_suite_runs_the_binary_that_preset_built(self):
        # bench.sh runs build/<preset>/bin/TaskSmackBenchmarks: the preset must be the one built.
        run = find_step(self.WORKFLOW, "Run benchmarks", job=self.JOB)
        self.assertIn("./tools/bench.sh benchmark ", run.run)


BENCH_LAUNCH_WRITER = r'''
import json, re, sys
from pathlib import Path

# argv: filter, launch number, output path. Writes the launch's timings from launches.json, the benchmarks
# the filter selects only, shaped as bench.sh writes them: 3 repetitions, then the aggregates.
pattern, number, out = sys.argv[1], sys.argv[2], Path(sys.argv[3])
stub = Path(__file__).parent
timings = json.loads((stub / "launches.json").read_text(encoding="utf-8"))[number]
with open(stub / "filters.log", "a", encoding="utf-8") as log:
    log.write(pattern + "\n")
rows = []
for name, time in timings.items():
    if re.search(pattern, name) is None:
        continue
    rows += [{"name": name, "run_name": name, "run_type": "iteration", "repetition_index": i, "iterations": 10,
              "real_time": time, "cpu_time": time, "time_unit": "ns"} for i in range(3)]
    rows += [{"name": f"{name}_{a}", "run_name": name, "run_type": "aggregate", "aggregate_name": a,
              "real_time": time, "cpu_time": time, "time_unit": "ns"} for a in ("mean", "median")]
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps({"context": {}, "benchmarks": rows}), encoding="utf-8")
'''

BENCH_SH_STUB = r"""#!/usr/bin/env bash
# tools/bench.sh's stand-in: one "launch" per call, numbered from 2, the filter passed through.
set -euo pipefail
[[ "$1" == "benchmark" && "$2" == "--" ]] || { echo "bench.sh stub: unexpected arguments $*" >&2; exit 2; }
filter=""
for arg in "$@"; do
  case "$arg" in --benchmark_filter=*) filter="${arg#--benchmark_filter=}" ;; esac
done
number=$(( $(cat "$STUB_DIR/count") + 1 ))
echo "$number" > "$STUB_DIR/count"
# A launch that overruns: launch STUB_SLOW_LAUNCH sleeps STUB_SLOW_SECONDS first.
if [[ "${STUB_SLOW_LAUNCH:-}" == "$number" ]]; then sleep "${STUB_SLOW_SECONDS:-30}"; fi
out="$TASKSMACK_BENCH_OUT_DIR/benchmark-$number.json"
"$STUB_PYTHON" "$STUB_DIR/write_launch.py" "$filter" "$number" "$out"
echo "RESULT=$out"
"""


class HeavyChecksAcrossLaunchesTests(unittest.TestCase):
    """#3476: a benchmark the gate flags is re-measured in 4 more separate launches, and the median across the 5
    launches is checked at the same thresholds. A slow launch passes; a real regression stays red."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"
    JOB = "benchmark-regression"
    CHECK = "Check for regression"
    REMEASURE = "Re-measure flagged benchmarks across launches"
    BASELINE = {"BM_HistoryBuffer_MemoryFootprint/iterations:10000": 4.3, "BM_Big": 1000.0, "BM_Steady": 50.0}

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.work = self.tmp / "work"
        self.stub = self.tmp / "stub"
        self.runner_temp = self.tmp / "runner"
        for directory in (self.work / "tools", self.work / "perf-data", self.stub, self.runner_temp):
            directory.mkdir(parents=True)
        for script in ("check-benchmark-regression.py", "bench-across-launches.py"):
            shutil.copy(REPO / "tools" / script, self.work / "tools" / script)
        bench_sh = self.work / "tools" / "bench.sh"
        bench_sh.write_text(BENCH_SH_STUB, encoding="utf-8", newline="\n")
        bench_sh.chmod(bench_sh.stat().st_mode | stat.S_IXUSR)
        (self.stub / "write_launch.py").write_text(BENCH_LAUNCH_WRITER, encoding="utf-8")
        # Files bash reads are written with LF on every platform (here, the counter and the STUB_DATES clocks): a
        # CRLF line from Python's Windows default would reach bash's arithmetic with its \r.
        (self.stub / "count").write_text("1\n", encoding="utf-8", newline="\n")
        shutil.copy(REPO / "perf-data" / "linux-ci-thresholds.json", self.work / "perf-data" / "linux-ci-thresholds.json")
        self.write_run("linux-ci-baseline.json", self.BASELINE)
        (self.work / "build" / "benchmark" / "bin").mkdir(parents=True)
        (self.work / "build" / "benchmark" / "bin" / "TaskSmackBenchmarks").write_text("binary\n", encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    def write_run(self, name: str, timings: dict[str, float]) -> None:
        rows = [{"name": f"{bm}_median", "run_name": bm, "aggregate_name": "median", "real_time": ns, "cpu_time": ns,
                 "time_unit": "ns"} for bm, ns in timings.items()]
        (self.work / "perf-data" / name).write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")

    def run_step(self, name: str, extra_env: dict[str, str] | None = None) -> tuple[subprocess.CompletedProcess, dict]:
        step = find_step(self.WORKFLOW, name, job=self.JOB)
        output = self.tmp / f"output-{name}"
        output.write_text("", encoding="utf-8")
        script = (
            # GitHub's bash runs a step with -e. python3 is this interpreter (Git Bash's may be the Store alias).
            "set -e\n"
            f'python3() {{ "{posix(Path(sys.executable))}" "$@"; }}\n'
            # With STUB_DATES, `date +%s` answers from that file, a line per call, so a deadline can pass
            # between two launches without waiting for it.
            'date() { if [[ -n "${STUB_DATES:-}" ]]; then head -n 1 "$STUB_DATES"; sed -i 1d "$STUB_DATES"; '
            'else command date "$@"; fi; }\n'
            f'cd "{posix(self.work)}"\n' + step.run
        )
        env = {key: value.strip("'") for key, value in step.env.items()}
        result = run_bash(script, self.tmp, {
            **env, "RUNNER_TEMP": posix(self.runner_temp), "GITHUB_OUTPUT": posix(output),
            "JOB_START_EPOCH": str(int(time.time())), "STUB_DIR": posix(self.stub), "STUB_PYTHON": posix(Path(sys.executable)),
            "PYTHONUTF8": "1", **(extra_env or {}),
        })
        outputs = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines() if "=" in line)
        return result, outputs

    def gate(self, first: dict[str, float], launches: list[dict[str, float]], extra_env: dict[str, str] | None = None):
        """Runs the check on `first`, then, when it flagged, the re-measurement with `launches` as launches 2-5."""
        self.write_run("benchmark-current.json", first)
        (self.stub / "launches.json").write_text(
            json.dumps({str(i + 2): timings for i, timings in enumerate(launches)}), encoding="utf-8")
        check, check_out = self.run_step(self.CHECK)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        remeasure, remeasure_out = self.run_step(self.REMEASURE, extra_env)
        return check_out["exit_code"], remeasure, remeasure_out

    def fails(self, check_exit: str, remeasure_exit: str | None, handed: str | None = None) -> bool:
        """The "Fail on regression" step's `if:`, as the workflow writes it."""
        condition = find_step(self.WORKFLOW, "Fail on regression", job=self.JOB).condition
        self.assertEqual(
            condition, "steps.regression-check.outputs.exit_code != '0' && steps.across-launches.outputs.exit_code != '0' "
                       "&& steps.handoff.outputs.handed != 'true'")
        return check_exit != "0" and (remeasure_exit or "") != "0" and (handed or "") != "true"

    HANDOFF = "Hand the standing flags to a second runner"

    def handoff(self, check_exit: str, remeasure_exit: str | None, forced: str = "") -> tuple[str | None, list[str] | None]:
        """Runs the handoff step when its `if:` holds (on a dispatch when `forced`): its `handed` output and the
        names it handed over."""
        step = find_step(self.WORKFLOW, self.HANDOFF, job=self.JOB)
        self.assertEqual(step.condition, (
            "(steps.regression-check.outputs.exit_code == '1' && steps.across-launches.outputs.exit_code == '1') || "
            "(steps.regression-check.outputs.exit_code == '0' && github.event_name == 'workflow_dispatch' && "
            "inputs.second_runner_benchmarks != '')"))
        if not ((check_exit == "1" and remeasure_exit == "1") or (check_exit == "0" and forced)):
            return None, None
        result, outputs = self.run_step(self.HANDOFF, {"FORCED_NAMES": forced})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        handed_dir = self.runner_temp / "second-runner"
        names = json.loads((handed_dir / "regressions.json").read_text(encoding="utf-8"))["regressions"]
        if outputs["handed"] == "true":
            self.assertTrue((handed_dir / "TaskSmackBenchmarks").is_file())
            self.assertTrue((handed_dir / "benchmark-current.json").is_file())
            # #3692: what this runner's gate judged goes too, for the second runner's other benchmarks.
            across = self.work / "perf-data" / "benchmark-across-launches.json"
            judged = across if across.is_file() else self.work / "perf-data" / "benchmark-current.json"
            self.assertEqual((handed_dir / "benchmark-judged.json").read_bytes(), judged.read_bytes())
        return outputs["handed"], names

    def test_the_remeasure_runs_only_on_a_flag_and_rechecks_at_the_same_thresholds(self):
        names = step_names(self.WORKFLOW, self.JOB)
        self.assertEqual(names.index(self.REMEASURE), names.index(self.CHECK) + 1)
        self.assertEqual(find_step(self.WORKFLOW, self.REMEASURE, job=self.JOB).condition,
                         "steps.regression-check.outputs.exit_code == '1'")
        check = find_step(self.WORKFLOW, self.CHECK, job=self.JOB).run
        remeasure = find_step(self.WORKFLOW, self.REMEASURE, job=self.JOB).run
        for argument in ("--baseline perf-data/linux-ci-baseline.json", "--threshold 40",
                         "--threshold-overrides perf-data/linux-ci-thresholds.json"):
            self.assertIn(argument, check)
            self.assertEqual(remeasure.count(argument), 1 if argument.startswith("--threshold") else 2, argument)
        self.assertNotIn("--min-coverage", remeasure, "the default coverage floor, as the first check")
        env = find_step(self.WORKFLOW, self.REMEASURE, job=self.JOB).env
        self.assertEqual(env["LAUNCHES"], "'4'", "5 launches in all, as decided on #3448")
        job = self.WORKFLOW.read_text(encoding="utf-8").split(f"\n  {self.JOB}:\n", 1)[1]
        timeout = re.search(r"^    timeout-minutes: (\d+)$", job, re.MULTILINE)
        self.assertEqual(int(env["JOB_TIMEOUT_SECONDS"].strip("'")), int(timeout.group(1)) * 60)

    def test_a_clean_run_never_remeasures(self):
        check_exit, remeasure, outputs = self.gate(dict(self.BASELINE), [])
        self.assertEqual(check_exit, "0")
        self.assertEqual(self.handoff(check_exit, None), (None, None), "nothing goes to the second runner")
        self.assertFalse(self.fails(check_exit, None))

    def test_a_dispatch_can_send_named_benchmarks_to_the_second_runner(self):
        check_exit, _, _ = self.gate(dict(self.BASELINE), [])
        handed, names = self.handoff(check_exit, None, forced=" BM_Big, BM_Steady ,")
        self.assertEqual((handed, names), ("true", ["BM_Big", "BM_Steady"]))
        self.assertFalse(self.fails(check_exit, None, handed))

    def test_one_slow_launch_passes(self):
        footprint = "BM_HistoryBuffer_MemoryFootprint/iterations:10000"
        # #3502: +52% (4.3 -> 6.6 ns) in one launch of a Windows-only change.
        first = {**self.BASELINE, footprint: 6.6}
        launches = [{footprint: t} for t in (4.4, 6.5, 4.2, 4.5)]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertEqual(check_exit, "1")
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "0", remeasure.stdout)
        self.assertFalse(self.fails(check_exit, outputs["exit_code"]))
        self.assertEqual((self.stub / "filters.log").read_text(encoding="utf-8").splitlines(),
                         ["^(BM_HistoryBuffer_MemoryFootprint/iterations:10000)$"] * 4, "4 launches of the flagged one")
        report = (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8")
        self.assertIn("launch 1: 6.60ns (+53.5%)", report)
        self.assertIn("median: 4.50ns (+4.7%)", report)
        self.assertIn("Coverage: 100.0% (3 of 3", report)

    def test_a_real_regression_stays_red(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": t} for t in (1580.0, 1620.0, 1010.0, 1590.0)]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(check_exit, outputs["exit_code"]))
        report = (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8")
        self.assertIn("median: 1590.00ns (+59.0%)", report)
        self.assertIn("FAILED: 1 benchmark(s) regressed", report)
        # #3623: every launch ran on this one host, so the flags that stand go to a second runner, which decides.
        handed, names = self.handoff(check_exit, outputs["exit_code"])
        self.assertEqual((handed, names), ("true", ["BM_Big"]))
        self.assertFalse(self.fails(check_exit, outputs["exit_code"], handed), "the second runner's verdict is the gate's")

    def test_only_the_flags_that_still_stand_are_handed_over(self):
        footprint = "BM_HistoryBuffer_MemoryFootprint/iterations:10000"
        first = {**self.BASELINE, "BM_Big": 1600.0, footprint: 6.6}
        launches = [{"BM_Big": t, footprint: 4.4} for t in (1580.0, 1620.0, 1010.0, 1590.0)]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertEqual(self.handoff(check_exit, outputs["exit_code"]), ("true", ["BM_Big"]))

    def test_what_the_gate_judged_goes_to_the_second_runner_not_the_first_launch(self):
        # #3692: BM_Steady read +60% in launch 1 and was cleared by the launches; BM_Big stood. The second runner
        # gets BM_Steady's median across the launches, not launch 1's value, so it isn't failed on it again.
        first = {**self.BASELINE, "BM_Big": 1600.0, "BM_Steady": 80.0}
        launches = [{"BM_Big": t, "BM_Steady": 51.0} for t in (1580.0, 1620.0, 1010.0, 1590.0)]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertEqual(self.handoff(check_exit, outputs["exit_code"]), ("true", ["BM_Big"]))
        judged = json.loads((self.runner_temp / "second-runner" / "benchmark-judged.json").read_text(encoding="utf-8"))
        steady = [row["real_time"] for row in judged["benchmarks"] if row.get("run_name") == "BM_Steady"]
        self.assertEqual(steady, [51.0])

    def test_a_coverage_loss_alone_is_not_handed_over(self):
        # BM_Steady missing: 2 of 3 is under the 90% floor, with no regression to re-measure.
        first = {name: ns for name, ns in self.BASELINE.items() if name != "BM_Steady"}
        check_exit, remeasure, outputs = self.gate(first, [])
        self.assertEqual(check_exit, "1")
        self.assertEqual(outputs["exit_code"], "1", "nothing to re-measure")
        handed, names = self.handoff(check_exit, outputs["exit_code"])
        self.assertEqual((handed, names), ("false", []))
        self.assertTrue(self.fails(check_exit, outputs["exit_code"], handed))

    def test_no_time_left_leaves_the_flag_standing(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        check_exit, remeasure, outputs = self.gate(first, [], {"JOB_START_EPOCH": str(int(time.time()) - 2700)})
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(check_exit, outputs["exit_code"]))
        self.assertFalse((self.stub / "filters.log").exists(), "no launch started")
        self.assertIn("flags stand as regressions", (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8"))
        # The second runner has a budget of its own.
        self.assertEqual(self.handoff(check_exit, outputs["exit_code"]), ("true", ["BM_Big"]))

    @unittest.skipIf(os.name == "nt", "timeout's process-group kill is POSIX; the step runs on Linux")
    def test_a_launch_past_the_deadline_is_stopped_and_the_flag_stands(self):
        # #3508: the estimate can be short, so each launch runs under timeout with what's left of the budget.
        # 4 s: the plan's estimate for one benchmark with no timed repetitions is 1 s a launch, 4 s in all.
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": 1000.0}] * 4
        # The step's two clock reads (its start, then before launch 2) are the same second, so launch 2 gets the
        # whole 4 s even when the real clock would tick between them. The real timeout still kills the launch.
        now = int(time.time())
        dates = self.tmp / "dates"
        dates.write_text(f"{now}\n{now}\n", encoding="utf-8", newline="\n")
        started = time.monotonic()
        check_exit, remeasure, outputs = self.gate(first, launches, {
            "MAX_SECONDS": "4", "STUB_SLOW_LAUNCH": "2", "STUB_SLOW_SECONDS": "30",
            "JOB_START_EPOCH": str(now), "STUB_DATES": posix(dates)})
        self.assertLess(time.monotonic() - started, 25, "the slow launch was killed, not waited out")
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(check_exit, outputs["exit_code"]))
        report = (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8")
        self.assertIn("Out of time: launch 2 of 5 stopped after 4s, past the 4s budget. The flags stand", report)
        self.assertFalse((self.work / "perf-data" / "launches" / "2").exists(), "the killed launch's partial output")
        self.assertFalse((self.work / "perf-data" / "benchmark-across-launches.json").exists())

    def test_the_deadline_passing_between_launches_stops_them(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": 1000.0}] * 4
        now = int(time.time())
        dates = self.tmp / "dates"
        # The step reads the clock once at its start, then before each launch: launch 2 starts with the whole
        # budget, and launch 3 finds it gone.
        dates.write_text(f"{now}\n{now}\n{now + 600}\n", encoding="utf-8", newline="\n")
        check_exit, remeasure, outputs = self.gate(
            first, launches, {"JOB_START_EPOCH": str(now), "STUB_DATES": posix(dates)})
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(check_exit, outputs["exit_code"]))
        self.assertEqual(len((self.stub / "filters.log").read_text(encoding="utf-8").splitlines()), 1, "launch 2 only")
        report = (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8")
        self.assertIn("Out of time: launch 3 of 5 not started, past the 600s budget. The flags stand", report)

    def test_a_failed_launch_fails_the_step(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        # Launch 3 doesn't measure it: the combine refuses, and the step fails.
        launches = [{"BM_Big": 1000.0}, {}, {"BM_Big": 1000.0}, {"BM_Big": 1000.0}]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertNotEqual(remeasure.returncode, 0, remeasure.stdout)
        self.assertNotIn("exit_code", outputs)
        self.assertIn("no usable median in launch 3", remeasure.stdout + remeasure.stderr)


class HeavyChecksSecondRunnerTests(unittest.TestCase):
    """#3623: flags that stood on benchmark-regression's runner are re-measured on a fresh runner, in 5 launches
    of the same binary, and that runner's median decides. A slow host can't fail the gate alone; a real
    regression, slow on any host, still fails it."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"
    JOB = "benchmark-second-runner"
    REMEASURE = "Re-measure the flagged benchmarks on this runner"
    BASELINE = {"BM_StorageModel_LatestSnapshot": 12.1, "BM_SystemModel_PerCoreHistory": 12.1, "BM_Big": 1000.0}

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.work = self.tmp / "work"
        self.stub = self.tmp / "stub"
        self.runner_temp = self.tmp / "runner"
        for directory in (self.work / "tools", self.work / "perf-data", self.work / "second-runner", self.stub,
                          self.runner_temp):
            directory.mkdir(parents=True)
        for script in ("check-benchmark-regression.py", "bench-across-launches.py"):
            shutil.copy(REPO / "tools" / script, self.work / "tools" / script)
        bench_sh = self.work / "tools" / "bench.sh"
        bench_sh.write_text(BENCH_SH_STUB, encoding="utf-8", newline="\n")
        bench_sh.chmod(bench_sh.stat().st_mode | stat.S_IXUSR)
        (self.stub / "write_launch.py").write_text(BENCH_LAUNCH_WRITER, encoding="utf-8")
        # This runner's launches are numbered from 1.
        (self.stub / "count").write_text("0\n", encoding="utf-8", newline="\n")
        shutil.copy(REPO / "perf-data" / "linux-ci-thresholds.json", self.work / "perf-data" / "linux-ci-thresholds.json")
        self.write_run(self.work / "perf-data" / "linux-ci-baseline.json", self.BASELINE)

    def tearDown(self):
        self._tmp.cleanup()

    @staticmethod
    def write_run(path: Path, timings: dict[str, float]) -> None:
        rows = [{"name": f"{bm}_median", "run_name": bm, "aggregate_name": "median", "real_time": ns, "cpu_time": ns,
                 "time_unit": "ns"} for bm, ns in timings.items()]
        path.write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")

    def second_runner(self, first: dict[str, float], flagged: list[str], launches: list[dict[str, float]],
                      extra_env: dict[str, str] | None = None, judged: dict[str, float] | None = None):
        """benchmark-regression handed over `first`, what its gate `judged` (`first` when not given) and `flagged`;
        `launches` are this runner's 5."""
        self.write_run(self.work / "second-runner" / "benchmark-current.json", first)
        self.write_run(self.work / "second-runner" / "benchmark-judged.json", first if judged is None else judged)
        (self.work / "second-runner" / "regressions.json").write_text(json.dumps({"regressions": flagged}), encoding="utf-8")
        (self.stub / "launches.json").write_text(
            json.dumps({str(i + 1): timings for i, timings in enumerate(launches)}), encoding="utf-8")
        step = find_step(self.WORKFLOW, self.REMEASURE, job=self.JOB)
        output = self.tmp / "output"
        output.write_text("", encoding="utf-8")
        script = (
            "set -e\n"
            f'python3() {{ "{posix(Path(sys.executable))}" "$@"; }}\n'
            f'cd "{posix(self.work)}"\n' + step.run
        )
        env = {key: value.strip("'") for key, value in step.env.items()}
        result = run_bash(script, self.tmp, {
            **env, "RUNNER_TEMP": posix(self.runner_temp), "GITHUB_OUTPUT": posix(output), "STUB_DIR": posix(self.stub),
            "STUB_PYTHON": posix(Path(sys.executable)), "PYTHONUTF8": "1", **(extra_env or {}),
        })
        outputs = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines() if "=" in line)
        report_path = self.work / "benchmark-second-runner-output.txt"
        report = report_path.read_text(encoding="utf-8") if report_path.exists() else ""
        return result, outputs, report

    def fails(self, exit_code: str | None) -> bool:
        condition = find_step(self.WORKFLOW, "Fail on regression", job=self.JOB).condition
        self.assertEqual(condition, "steps.second-runner.outputs.exit_code != '0'")
        return (exit_code or "") != "0"

    def test_the_job_runs_only_on_a_handoff_and_checks_at_the_same_limits(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split(f"\n  {self.JOB}:\n", 1)[1].split("\n  # ", 1)[0]
        self.assertIn("    needs: benchmark-regression\n", job)
        self.assertIn("    if: needs.benchmark-regression.outputs.second-runner == 'true'\n", job)
        first_job = text.split("\n  benchmark-regression:\n", 1)[1]
        self.assertIn("      second-runner: ${{ steps.handoff.outputs.handed }}\n", first_job)
        remeasure = find_step(self.WORKFLOW, self.REMEASURE, job=self.JOB)
        for argument in ("--threshold 40", "--threshold-overrides perf-data/linux-ci-thresholds.json", "--exclude-first"):
            self.assertEqual(remeasure.run.count(argument), 1, argument)
        self.assertEqual(remeasure.run.count("--baseline perf-data/linux-ci-baseline.json"), 2)
        self.assertNotIn("--min-coverage", remeasure.run)
        self.assertEqual(remeasure.env["LAUNCHES"], "'5'")
        timeout = int(re.search(r"^    timeout-minutes: (\d+)$", job, re.MULTILINE).group(1))
        self.assertLess(int(remeasure.env["MAX_SECONDS"].strip("'")), timeout * 60 - 300, "room for setup and upload")

    def test_a_slow_first_host_passes_on_a_normal_one(self):
        # #3623: 12.1 -> 17.9 ns on an Intel Xeon host for #3617's test-only change; ~12 ns on the EPYC ones.
        flagged = ["BM_StorageModel_LatestSnapshot", "BM_SystemModel_PerCoreHistory"]
        first = {**self.BASELINE, flagged[0]: 17.9, flagged[1]: 17.3}
        launches = [{flagged[0]: t, flagged[1]: t} for t in (12.2, 12.0, 17.5, 12.3, 12.1)]
        result, outputs, report = self.second_runner(first, flagged, launches)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outputs["exit_code"], "0", report)
        self.assertFalse(self.fails(outputs["exit_code"]))
        self.assertEqual(len((self.stub / "filters.log").read_text(encoding="utf-8").splitlines()), 5, "5 launches here")
        self.assertIn("Median across 5 separate launches on this runner (the first runner's values not counted)", report)
        self.assertNotIn("17.90ns", report, "the first host's value isn't one of the launches")
        self.assertIn("median: 12.20ns (+0.8%)", report)
        self.assertIn("Coverage: 100.0% (3 of 3", report)

    def test_a_benchmark_the_first_runner_cleared_is_judged_on_its_launches_median(self):
        # #3692: BM_Big read +48% in the first runner's launch 1, and its launches cleared it (+3%); only
        # BM_StorageModel_LatestSnapshot stood. The other benchmarks come from what that gate judged.
        flagged = ["BM_StorageModel_LatestSnapshot"]
        first = {**self.BASELINE, "BM_Big": 1480.0, flagged[0]: 17.9}
        judged = {**self.BASELINE, "BM_Big": 1030.0, flagged[0]: 17.5}
        launches = [{flagged[0]: t} for t in (12.2, 12.0, 12.3, 12.1, 12.2)]
        result, outputs, report = self.second_runner(first, flagged, launches, judged=judged)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outputs["exit_code"], "0", report)
        self.assertNotIn("BM_Big: 1000.0ns -> 1480.0ns", report)
        self.assertIn("Coverage: 100.0% (3 of 3", report)

        # Judged on the first launch alone, as before #3692's fix, it failed.
        (self.stub / "count").write_text("0\n", encoding="utf-8", newline="\n")  # this runner's launches, from 1 again
        result, outputs, report = self.second_runner(first, flagged, launches, judged=first)
        self.assertEqual(outputs["exit_code"], "1", report)
        self.assertIn("BM_Big", report)

    def test_a_real_regression_fails_on_the_second_runner_too(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": t} for t in (1580.0, 1620.0, 1010.0, 1590.0, 1610.0)]
        result, outputs, report = self.second_runner(first, ["BM_Big"], launches)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(outputs["exit_code"]))
        self.assertIn("FAILED: 1 benchmark(s) regressed", report)

    def test_over_budget_the_flags_stand(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        # 1 s a launch for a benchmark with no timed repetitions, 5 s for 5: over a 4 s budget.
        result, outputs, report = self.second_runner(first, ["BM_Big"], [], {"MAX_SECONDS": "4"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(outputs["exit_code"]))
        self.assertFalse((self.stub / "filters.log").exists(), "no launch started")
        self.assertIn("flags stand as regressions", report)

    def test_a_failed_launch_fails_the_step(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": 1000.0}, {}, {"BM_Big": 1000.0}, {"BM_Big": 1000.0}, {"BM_Big": 1000.0}]
        result, outputs, report = self.second_runner(first, ["BM_Big"], launches)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("exit_code", outputs)
        self.assertIn("no usable median in launch 2", result.stdout + result.stderr)


class RenovateDryRunTests(unittest.TestCase):
    """#2527: RENOVATE_DRY_RUN=disabled still made every run a dry run, so no dependency PR opened."""

    WORKFLOW = WORKFLOWS / "renovate.yml"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.step = find_step(self.WORKFLOW, "Select dry-run mode")

    def tearDown(self):
        self._tmp.cleanup()

    def select(self, dry_run_input: str) -> tuple[subprocess.CompletedProcess, str]:
        github_env = self.tmp / "github_env"
        github_env.write_text("", encoding="utf-8")
        result = run_bash(
            self.step.run, self.tmp, {"DRY_RUN_INPUT": dry_run_input, "GITHUB_ENV": posix(github_env)}
        )
        return result, github_env.read_text(encoding="utf-8")

    def test_a_scheduled_or_disabled_run_sets_no_dry_run(self):
        for value in ("", "disabled"):
            with self.subTest(dry_run_input=value):
                result, exported = self.select(value)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("RENOVATE_DRY_RUN", exported)

    def test_an_explicit_dry_run_dispatch_exports_its_mode(self):
        for value in ("extract", "lookup", "full"):
            with self.subTest(dry_run_input=value):
                result, exported = self.select(value)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(exported.strip(), f"RENOVATE_DRY_RUN={value}")

    def test_an_unknown_mode_fails(self):
        result, exported = self.select("bogus")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("::error::", result.stdout)
        self.assertNotIn("RENOVATE_DRY_RUN", exported)

    def test_only_a_dispatch_reads_the_input(self):
        # Scheduled runs have no inputs; the expression must not fall back to a mode of its own.
        expression = self.step.env.get("DRY_RUN_INPUT", "")
        self.assertIn("github.event_name == 'workflow_dispatch'", expression)
        self.assertIn("|| ''", expression)

    def test_the_renovate_step_does_not_set_dry_run_itself(self):
        renovate = find_step(self.WORKFLOW, "Self-hosted Renovate")
        self.assertNotIn("RENOVATE_DRY_RUN", renovate.env)


class SanitizerSummaryTests(unittest.TestCase):
    """#2532: `sanitizer-reports/<preset>.html` inside double quotes ran the report as a command."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        step = find_step(WORKFLOWS / "sanitizers.yml", "Add sanitizer summary to workflow summary")
        self.script = step.run

    def tearDown(self):
        self._tmp.cleanup()

    def summarize(self, test_result: str) -> tuple[subprocess.CompletedProcess, str]:
        (self.tmp / "sanitizer-logs").mkdir()
        (self.tmp / "sanitizer-logs" / "tsan.log").write_text(
            "WARNING: ThreadSanitizer: data race\nSUMMARY: ThreadSanitizer: data race in f()\n",
            encoding="utf-8",
            newline="\n",
        )
        # The report the summary names exists and is executable-looking, as in CI: running it as a
        # command must still not happen.
        (self.tmp / "sanitizer-reports").mkdir()
        report = self.tmp / "sanitizer-reports" / "tsan.html"
        report.write_text("<html></html>\n", encoding="utf-8")
        summary = self.tmp / "summary.md"
        script = substitute(
            self.script,
            {"matrix.preset": "tsan", "steps.sanitizer-test.outputs.test_result": test_result},
        )
        result = run_bash(script, self.tmp, {"GITHUB_STEP_SUMMARY": posix(summary)})
        return result, summary.read_text(encoding="utf-8")

    def test_the_summary_names_the_report_and_runs_nothing(self):
        for test_result in ("0", "1"):
            with self.subTest(test_result=test_result):
                if (self.tmp / "sanitizer-logs").exists():
                    shutil.rmtree(self.tmp / "sanitizer-logs")
                    shutil.rmtree(self.tmp / "sanitizer-reports")
                result, summary = self.summarize(test_result)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertIn("Full report available in artifacts: `sanitizer-reports/tsan.html`\n", summary)

    def test_a_failed_run_lists_the_log_findings(self):
        _, summary = self.summarize("1")
        self.assertIn("FAILED", summary)
        self.assertIn("SUMMARY: ThreadSanitizer: data race in f()", summary)


# Stands in for `gh api URL [--jq FILTER] ...`: answers workflow-run and job listings from
# $FIXTURES, through the caller's own jq filter. Any other call fails the test.
GH_STUB = r"""#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == api ]] || { echo "gh stub: unexpected command $*" >&2; exit 2; }
shift
url=""
filter=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --jq) filter="$2"; shift 2 ;;
    -*) shift ;;
    *) [[ -n "$url" ]] || url="$1"; shift ;;
  esac
done
case "$url" in
  repos/*/actions/workflows/*/runs\?*)
    workflow="${url#*/workflows/}"; workflow="${workflow%%/runs*}"
    event="${url#*event=}"; event="${event%%&*}"
    file="$FIXTURES/runs_${workflow}_${event}.json"
    [[ -f "$file" ]] || file="$FIXTURES/no_runs.json" ;;
  repos/*/actions/runs/*/jobs*)
    id="${url#*/runs/}"; id="${id%%/jobs*}"
    file="$FIXTURES/jobs_${id}.json"
    [[ -f "$file" ]] || file="$FIXTURES/no_jobs.json" ;;
  repos/*/branches\?*) file="$FIXTURES/branches.json" ;;
  *) echo "gh stub: unexpected api call $url" >&2; exit 2 ;;
esac
if [[ -n "$filter" ]]; then jq -r "$filter" "$file"; else cat "$file"; fi
"""

UNITY = "build-linux-unity"
RED_MARK = "===RED==="


def bash_has_jq() -> bool:
    try:
        result = subprocess.run([BASH, "-c", "command -v jq"], capture_output=True, check=False, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


MAIN_HEALTH_STEP = "Open, update or close the tracking issue"

# Stands in for the issue calls of update_issue(): logs each call to $FIXTURES/calls.log, finds
# tracking issue #7 open and answers its body from $FIXTURES/previous_body.txt.
ISSUE_GH_STUB = r"""#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FIXTURES/calls.log"
case "$*" in
  "api repos/owner/repo/issues?labels="*) echo 7 ;;
  "api repos/owner/repo/issues/7 --jq .body") cat "$FIXTURES/previous_body.txt" ;;
  "api -X PATCH repos/owner/repo/issues/7 "*|"api repos/owner/repo/issues/7/comments "*) ;;
  *) echo "gh stub: unexpected command $*" >&2; exit 2 ;;
esac
"""


@unittest.skipUnless(bash_has_jq(), "jq is not on bash's PATH")
class MainHealthBranchTests(unittest.TestCase):
    """#2951: Main Health's one concurrency group keeps a single pending run and replaces it, so a run
    that judged only its triggering branch could drop another branch's update. Every run now judges
    main and every release/* branch, and only the triggering branch's issue gets the turned-red comment."""

    BRANCHES = ["main", "release/0.9", "release/0.10"]

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.fixtures = self.tmp / "fixtures"
        self.fixtures.mkdir()
        step = find_step(WORKFLOWS / "main-health.yml", MAIN_HEALTH_STEP)
        # The step's functions end with update_issue(); the branch selection and loop follow it.
        cut = step.run.index("\n}\n", step.run.index("update_issue() {")) + 3
        self.functions, self.selection = step.run[:cut], step.run[cut:]
        branches = [{"name": name} for name in ("fix/2951-main-health", "main", "release/0.10", "release/0.9")]
        (self.fixtures / "branches.json").write_text(json.dumps(branches), encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    def stub(self, name: str, text: str) -> str:
        """A `gh` shell function running the stub, so the real gh can never answer (see MainHealthJobTests)."""
        path = self.tmp / name
        path.write_text(text, encoding="utf-8", newline="\n")
        return f'gh() {{ "$BASH" "{posix(path)}" "$@"; }}\n'

    def trigger_env(self, event: str, branch: str, conclusion: str) -> dict[str, str]:
        workflow_run = event == "workflow_run"
        return {
            "FIXTURES": posix(self.fixtures),
            "REPO": "owner/repo",
            "SERVER": "https://github.example",
            "LABEL": "ci-red-main",
            "GITHUB_WORKFLOW": "Main Health",
            "GITHUB_RUN_ID": "42",
            "EVENT_NAME": event,
            "TRIGGER_BRANCH": branch if workflow_run else "",
            "TRIGGER_NAME": "CI" if workflow_run else "",
            "TRIGGER_URL": "https://github.example/runs/1" if workflow_run else "",
            "TRIGGER_CONCLUSION": conclusion if workflow_run else "",
            "TRIGGER_SHA": "0123456789abcdef" if workflow_run else "",
        }

    def select(self, event: str, trigger_branch: str) -> subprocess.CompletedProcess:
        """The step's branch selection and loop, with judge_branch and update_issue only printing the branch."""
        script = (
            "set -euo pipefail\n"
            + self.stub("gh", GH_STUB)
            + 'judge_branch() { red=""; echo "JUDGE $1"; }\n'
            + 'update_issue() { echo "UPDATE $1"; }\n'
            + self.selection
        )
        return run_bash(script, self.tmp, self.trigger_env(event, trigger_branch, "failure"))

    def judged(self, event: str, trigger_branch: str) -> list[str]:
        """The branches the step judges and updates, in order, for one triggering event."""
        result = self.select(event, trigger_branch)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line for line in result.stdout.splitlines() if line.startswith(("JUDGE ", "UPDATE "))]
        judged = [line.removeprefix("JUDGE ") for line in lines[0::2]]
        self.assertEqual(lines[1::2], [f"UPDATE {name}" for name in judged], "each judged branch is updated next")
        return judged

    def test_no_release_branch_judges_main_alone(self):
        (self.fixtures / "branches.json").write_text(json.dumps([{"name": "main"}]), encoding="utf-8")
        self.assertEqual(self.judged("workflow_run", "main"), ["main"])

    def test_a_failed_branch_listing_fails_the_step(self):
        # Judging main alone would leave a release branch's issue as it was without saying so.
        (self.fixtures / "branches.json").unlink()
        result = self.select("workflow_run", "release/0.9")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("JUDGE ", result.stdout)

    def test_every_run_judges_main_and_every_release_branch(self):
        for event, trigger_branch in (
            ("workflow_run", "main"),
            ("workflow_run", "release/0.9"),
            ("workflow_run", "release/0.10"),
            ("workflow_dispatch", ""),
        ):
            with self.subTest(event=event, trigger_branch=trigger_branch):
                self.assertEqual(sorted(self.judged(event, trigger_branch)), sorted(self.BRANCHES))

    def comments_on_update(self, trigger_branch: str, branch: str) -> list[str]:
        """The issue calls update_issue makes for `branch`'s open, red issue after a CI failure on `trigger_branch`."""
        (self.fixtures / "previous_body.txt").write_text("- **Sanitizers** (push): `failure`\n", encoding="utf-8")
        calls = self.fixtures / "calls.log"
        calls.write_text("", encoding="utf-8")
        script = (
            self.functions
            + self.stub("gh_issues", ISSUE_GH_STUB)
            + "red='- **CI** (push): `failure`'$'\\n'\n"
            + f'update_issue "{branch}"\n'
        )
        result = run_bash(script, self.tmp, self.trigger_env("workflow_run", trigger_branch, "failure"))
        self.assertEqual(result.returncode, 0, result.stderr)
        made = calls.read_text(encoding="utf-8").splitlines()
        self.assertTrue(any(call.startswith("api -X PATCH repos/owner/repo/issues/7 ") for call in made), made)
        return [call for call in made if call.startswith("api repos/owner/repo/issues/7/comments ")]

    def test_only_the_triggering_branch_gets_the_turned_red_comment(self):
        for trigger_branch, branch, commented in (
            ("main", "main", True),
            ("main", "release/0.9", False),
            ("release/0.9", "release/0.9", True),
            ("release/0.9", "main", False),
        ):
            with self.subTest(trigger_branch=trigger_branch, branch=branch):
                comments = self.comments_on_update(trigger_branch, branch)
                self.assertEqual(len(comments), 1 if commented else 0, comments)
                if commented:
                    self.assertIn("**CI** turned red: failure on `01234567`", comments[0])


@unittest.skipUnless(bash_has_jq(), "jq is not on bash's PATH")
class MainHealthJobTests(unittest.TestCase):
    """#2313: the weekday nightly skips the unity build, so it must not clear Sunday's unity failure."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.fixtures = self.tmp / "fixtures"
        self.fixtures.mkdir()
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        stub = bin_dir / "gh"
        stub.write_text(GH_STUB, encoding="utf-8", newline="\n")
        stub.chmod(stub.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        self.bin_dir = bin_dir
        self.write("no_runs.json", {"workflow_runs": []})
        self.write("no_jobs.json", {"jobs": []})

        step = find_step(WORKFLOWS / "main-health.yml", "Open, update or close the tracking issue")
        self.env = step.env
        # The judging function alone: cut before update_issue(), then judge main and print `red`.
        cut = step.run.index("update_issue() {")
        self.script = step.run[:cut] + f'judge_branch main\nprintf "%s\\n%s" "{RED_MARK}" "$red"\n'

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name: str, data: object) -> None:
        (self.fixtures / name).write_text(json.dumps(data), encoding="utf-8")

    def nightlies(self, runs: list[tuple[str, list[tuple[str, str | None]]]]) -> None:
        """CI's schedule runs, newest first: each a (run conclusion, [(job name, job conclusion)])."""
        listed = []
        for index, (conclusion, jobs) in enumerate(runs):
            run_id = 1000 - index
            listed.append(
                {
                    "id": run_id,
                    "conclusion": conclusion,
                    "html_url": f"https://example.invalid/runs/{run_id}",
                    "head_sha": f"{run_id:040d}",
                    "created_at": f"2026-10-{18 - index:02d}T06:00:00Z",
                }
            )
            self.write(
                f"jobs_{run_id}.json",
                {"jobs": [{"name": name, "conclusion": job_conclusion} for name, job_conclusion in jobs]},
            )
        self.write("runs_ci.yml_schedule.json", {"workflow_runs": listed})

    def red(self) -> str:
        self.assertIn("WATCHED_JOBS", self.env, "Main Health must judge CI's unity job on its own (#2313)")
        self.assertIn(f"ci.yml|CI|schedule|{UNITY}", self.env["WATCHED_JOBS"])
        # A shell function rather than a PATH entry, so the real gh can never answer, whichever bash
        # (Git Bash rebuilds PATH from the Windows one) runs the script.
        script = f'gh() {{ "$BASH" "{posix(self.bin_dir / "gh")}" "$@"; }}\n' + self.script
        env = {
            "FIXTURES": posix(self.fixtures),
            "REPO": "owner/repo",
            "WATCHED": self.env["WATCHED"],
            "WATCHED_JOBS": self.env["WATCHED_JOBS"],
        }
        result = run_bash(script, self.tmp, env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(RED_MARK, result.stdout)
        return result.stdout.split(RED_MARK, 1)[1]

    def test_a_unity_failure_stays_red_after_a_nightly_that_skipped_it(self):
        self.nightlies(
            [
                ("success", [("build-linux", "success"), (UNITY, "skipped")]),
                ("failure", [("build-linux", "success"), (f"{UNITY} / Build & Test (unity)", "failure")]),
            ]
        )
        red = self.red()
        self.assertIn(f"**CI** ({UNITY})", red)
        self.assertIn("`failure`", red)

    def test_a_later_unity_pass_clears_it(self):
        self.nightlies([("success", [(UNITY, "success")]), ("failure", [(UNITY, "failure")])])
        self.assertNotIn(UNITY, self.red())

    def test_no_run_that_ran_unity_changes_nothing(self):
        self.nightlies([("success", [(UNITY, "skipped")]), ("success", [(UNITY, "skipped")])])
        self.assertNotIn(UNITY, self.red())

    def test_the_newest_unity_failure_is_red(self):
        self.nightlies([("failure", [(UNITY, "failure")]), ("success", [(UNITY, "success")])])
        red = self.red()
        self.assertIn(f"({UNITY})", red)
        # The run itself failed too, so the workflow-level entry is listed as well.
        self.assertIn("**CI** (schedule)", red)

    def test_cancelled_and_skipped_runs_neither_clear_nor_set_it(self):
        self.nightlies(
            [
                ("success", [(UNITY, "cancelled")]),
                ("success", [(UNITY, "skipped")]),
                ("failure", [(UNITY, "failure")]),
            ]
        )
        self.assertIn(f"({UNITY})", self.red())


@unittest.skipUnless(PWSH, "pwsh is not on PATH")
class WindowsReproducibilityCompareTests(unittest.TestCase):
    """#2952: identical packages failed the job when the build-inputs manifests differed, because
    compare-build-inputs.ps1's exit 1 was still $LASTEXITCODE when the step ended."""

    COMPARATOR = REPO / ".github" / "actions" / "windows-build-inputs" / "compare-build-inputs.ps1"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        step = find_step(WORKFLOWS / "release.yml", "Compare digests", job="verify-reproducibility-windows")
        self.script = step.run
        comparator = self.tmp / ".github" / "actions" / "windows-build-inputs" / self.COMPARATOR.name
        comparator.parent.mkdir(parents=True)
        shutil.copyfile(self.COMPARATOR, comparator)
        for directory in ("dist", "original-dist", "build-inputs", "original-build-inputs"):
            (self.tmp / directory).mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def package(self, directory: str, exe: bytes) -> None:
        with zipfile.ZipFile(self.tmp / directory / "TaskSmack-1.0.0-windows-x64.zip", "w") as archive:
            for name, data in (("TaskSmack/bin/TaskSmack.exe", exe), ("TaskSmack/README.md", b"readme\n")):
                archive.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 25, 0, 0, 0)), data)

    def manifest(self, directory: str, name: str, runner_image: str | None) -> None:
        manifest = {"provenance": {}}
        if runner_image is not None:
            manifest["inputs"] = {"runner_image": runner_image, "toolset": "14.44"}
        (self.tmp / directory / f"{name}-build-inputs.json").write_text(json.dumps(manifest), encoding="utf-8")

    def compare(self, *, same_package: bool, original_image: str | None, rebuilt_image: str | None):
        self.package("original-dist", b"exe")
        self.package("dist", b"exe" if same_package else b"other exe")
        self.manifest("original-build-inputs", "a", original_image)
        self.manifest("build-inputs", "b", rebuilt_image)
        # What `shell: pwsh` runs: the step with $ErrorActionPreference set first and the last
        # native or script exit code returned at the end, dot-sourced by `pwsh -command`, which (as
        # on GitHub) turns any nonzero exit into 1.
        path = self.tmp / "step.ps1"
        path.write_text(
            "$ErrorActionPreference = 'stop'\n"
            + self.script
            + "\nif ((Test-Path -LiteralPath variable:\\LASTEXITCODE)) { exit $LASTEXITCODE }\n",
            encoding="utf-8",
        )
        return subprocess.run(
            [PWSH, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", f". '{path}'"],
            cwd=self.tmp,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout=120,
        )

    def test_identical_packages_and_inputs_pass(self):
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image="image-A")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Build inputs match", result.stdout)
        self.assertIn("All Windows packages are reproducible", result.stdout)

    def test_identical_packages_with_different_inputs_pass(self):
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image="image-B")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("::notice::The packages match although the build inputs differ", result.stdout)
        self.assertIn("All Windows packages are reproducible", result.stdout)

    def test_identical_packages_with_an_unreadable_manifest_pass(self):
        # The comparator exits 2; the packages still match.
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image=None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("::warning::Could not compare the build-inputs manifests", result.stdout)
        self.assertIn("All Windows packages are reproducible", result.stdout)

    def installer(self, directory: str, data: bytes) -> None:
        (self.tmp / directory / "TaskSmack-1.0.0-windows-x64.exe").write_bytes(data)

    def test_identical_installers_pass(self):
        # #2302: the NSIS installer's first pass is compared beside the .zip.
        self.installer("original-dist", b"installer")
        self.installer("dist", b"installer")
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image="image-A")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("All Windows packages are reproducible", result.stdout)

    def test_a_different_installer_fails(self):
        # An installer can't be opened to compare file by file, so any difference is one.
        self.installer("original-dist", b"installer")
        self.installer("dist", b"other installer")
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image="image-A")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("::warning::TaskSmack-1.0.0-windows-x64.exe differs between the two independent builds", result.stdout)
        self.assertNotIn("All Windows packages are reproducible", result.stdout)

    def test_an_installer_without_an_original_fails(self):
        self.installer("dist", b"installer")
        result = self.compare(same_package=True, original_image="image-A", rebuilt_image="image-A")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("::warning::No matching original artifact for TaskSmack-1.0.0-windows-x64.exe", result.stdout)

    def test_different_packages_fail(self):
        for rebuilt_image, explanation in (
            ("image-A", "::warning::The build inputs match"),
            ("image-B", "::warning::The build inputs differ too"),
        ):
            with self.subTest(rebuilt_image=rebuilt_image):
                for directory in ("dist", "original-dist", "build-inputs", "original-build-inputs"):
                    for leftover in (self.tmp / directory).iterdir():
                        leftover.unlink()
                result = self.compare(same_package=False, original_image="image-A", rebuilt_image=rebuilt_image)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("::warning::TaskSmack/bin/TaskSmack.exe differs", result.stdout)
                self.assertIn(explanation, result.stdout)
                self.assertNotIn("All Windows packages are reproducible", result.stdout)


THIRD_PARTY = ".cache/fetchcontent/tomlplusplus-src/include/toml++/impl/parser.inl"


def sarif_location(uri: str, line: int) -> dict:
    return {
        "physicalLocation": {
            "artifactLocation": {"uri": uri, "uriBaseId": "%SRCROOT%"},
            "region": {"startLine": line},
        }
    }


def sarif_flow(*uris: str) -> list[dict]:
    """One code flow with one thread flow whose steps are at `uris`, in order."""
    return [{"threadFlows": [{"locations": [{"location": sarif_location(uri, 10 + i)} for i, uri in enumerate(uris)]}]}]


@unittest.skipUnless(bash_has_jq(), "jq is not on bash's PATH")
class CodeqlSourceFilterTests(unittest.TestCase):
    """#2992: the filter kept a result only when locations[0] was under src/, so a taint flow from
    src/ into a sink in a FetchContent header was dropped, code flows and all."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        step = find_step(WORKFLOWS / "codeql.yml", "Keep results under src/", job="analyze")
        self.script = substitute(step.run, {})
        (self.tmp / "sarif-results").mkdir()
        self.sarif = self.tmp / "sarif-results" / "cpp.sarif"

    def tearDown(self):
        self._tmp.cleanup()

    def filtered(self, results: list[dict]) -> tuple[subprocess.CompletedProcess, list[str]]:
        """Runs the step on one run holding `results`; returns the run and the kept results' ruleIds."""
        artifacts = [{"location": {"uri": "src/UI/Theme.cpp"}}, {"location": {"uri": THIRD_PARTY}}]
        run = {"tool": {"driver": {"name": "CodeQL"}}, "artifacts": artifacts, "results": results}
        document = {"version": "2.1.0", "runs": [run]}
        self.sarif.write_text(json.dumps(document), encoding="utf-8", newline="\n")
        result = run_bash(self.script, self.tmp, {})
        kept = json.loads(self.sarif.read_text(encoding="utf-8"))
        self.assertEqual(kept["runs"][0]["artifacts"], artifacts, "the step must leave the run's artifacts alone")
        return result, [entry["ruleId"] for entry in kept["runs"][0]["results"]]

    def test_a_result_with_any_location_under_src_is_kept(self):
        results = [
            {"ruleId": "src-primary", "locations": [sarif_location("src/Domain/ProcessModel.cpp", 42)]},
            # A path-problem result: the sink (locations[0]) is in a third-party header, the source in src/.
            {
                "ruleId": "third-party-sink-src-flow",
                "locations": [sarif_location(THIRD_PARTY, 900)],
                "codeFlows": sarif_flow("src/UI/Theme.cpp", THIRD_PARTY),
            },
            {
                "ruleId": "third-party-only",
                "locations": [sarif_location(THIRD_PARTY, 120)],
                "relatedLocations": [sarif_location(THIRD_PARTY, 80)],
                "codeFlows": sarif_flow(THIRD_PARTY, THIRD_PARTY),
            },
            {"ruleId": "no-locations"},
        ]
        result, kept = self.filtered(results)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(kept, ["src-primary", "third-party-sink-src-flow"])
        self.assertIn("kept 2 of 4 results", result.stdout)
        self.assertIn("dropped 2 with no location under src/", result.stdout)

    def test_a_src_related_location_keeps_a_third_party_result(self):
        results = [
            {
                "ruleId": "third-party-primary-src-related",
                "locations": [sarif_location(THIRD_PARTY, 300)],
                "relatedLocations": [sarif_location("src/Platform/Windows/WindowsProcessProbe.cpp", 7)],
            },
            {"ruleId": "empty-arrays", "locations": [], "relatedLocations": [], "codeFlows": []},
        ]
        result, kept = self.filtered(results)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(kept, ["third-party-primary-src-related"])
        self.assertIn("kept 1 of 2 results", result.stdout)

    def test_no_sarif_file_fails_the_step(self):
        result = run_bash(self.script, self.tmp, {})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no SARIF file in sarif-results/", result.stdout)



class ReleasePackageSetTests(unittest.TestCase):
    """release.yml create-release "Verify the package set": the NSIS installers are part of the exact set (#2302)."""

    PACKAGES = (
        "Linux.deb",
        "Linux.tar.gz",
        "win64.zip",
        "win64.exe",
        "linux-aarch64.deb",
        "linux-aarch64.tar.gz",
        "windows-arm64.zip",
        "windows-arm64.exe",
        "win64-build-inputs.json",
        "windows-arm64-build-inputs.json",
    )

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.script = find_step(WORKFLOWS / "release.yml", "Verify the package set", job="create-release").run
        (self.tmp / "CMakeLists.txt").write_text("project(TaskSmack VERSION 1.2.3 LANGUAGES CXX)\n", encoding="utf-8")
        (self.tmp / "dist").mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def check(self, suffixes) -> subprocess.CompletedProcess:
        for suffix in suffixes:
            (self.tmp / "dist" / f"TaskSmack-1.2.3-{suffix}").write_bytes(b"package")
        return run_bash(self.script, self.tmp, {})

    def test_the_full_set_passes(self):
        result = self.check(self.PACKAGES)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TaskSmack-1.2.3-windows-arm64.exe", result.stdout)

    def test_a_missing_installer_fails(self):
        for missing in ("win64.exe", "windows-arm64.exe"):
            with self.subTest(missing=missing):
                for leftover in (self.tmp / "dist").iterdir():
                    leftover.unlink()
                result = self.check([suffix for suffix in self.PACKAGES if suffix != missing])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"< TaskSmack-1.2.3-{missing}", result.stderr)

    def test_a_sigstore_bundle_fails(self):
        result = self.check([*self.PACKAGES, "win64.zip.bundle"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("> TaskSmack-1.2.3-win64.zip.bundle", result.stderr)


class ReleaseSha256SumsTests(unittest.TestCase):
    """release.yml create-release "Write SHA256SUMS" and "Verify the release's file set" (#3145): one
    `sha256sum`-format SHA256SUMS lists every release file, and the published set is those files,
    SHA256SUMS and its one Sigstore bundle, with no per-file bundle."""

    LABEL = "v1.2.3"
    FILES = (
        "TaskSmack-1.2.3-Linux.deb",
        "TaskSmack-1.2.3-win64.zip",
        "TaskSmack-1.2.3-windows-arm64-build-inputs.json",
        "tasksmack-v1.2.3-source-sbom.spdx.json",
    )

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.dist = self.tmp / "dist"
        self.dist.mkdir()
        release = WORKFLOWS / "release.yml"
        self.write = find_step(release, "Write SHA256SUMS", job="create-release").run
        verify = find_step(release, "Verify the release's file set", job="create-release")
        self.verify = verify.run
        self.verify_env = {
            name: substitute(value, {"steps.version.outputs.label": self.LABEL}) for name, value in verify.env.items()
        }
        for index, name in enumerate(self.FILES):
            # Different sizes and bytes, a CRLF and a trailing newline among them, so a text-mode hash would differ.
            (self.dist / name).write_bytes(f"{name}\r\n".encode() * (index + 1))

    def tearDown(self):
        self._tmp.cleanup()

    def write_sums(self) -> subprocess.CompletedProcess:
        result = run_bash(self.write, self.tmp, {})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def signed_set(self):
        """dist/ as it is after signing: the files, SHA256SUMS and a stand-in SHA256SUMS.bundle."""
        self.write_sums()
        (self.dist / "SHA256SUMS.bundle").write_text("{}", encoding="utf-8")

    def verify_set(self) -> subprocess.CompletedProcess:
        return run_bash(self.verify, self.tmp, self.verify_env)

    def test_sha256sums_lists_every_file_in_sha256sum_format(self):
        self.write_sums()
        data = (self.dist / "SHA256SUMS").read_bytes()
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"\n"))
        expected = "".join(
            f"{hashlib.sha256((self.dist / name).read_bytes()).hexdigest()}  {name}\n" for name in sorted(self.FILES)
        )
        self.assertEqual(data.decode("utf-8"), expected)

    def test_sha256sums_fails_on_an_empty_dist(self):
        for name in self.FILES:
            (self.dist / name).unlink()
        result = run_bash(self.write, self.tmp, {})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertFalse((self.dist / "SHA256SUMS").exists())

    def test_the_signed_set_passes(self):
        self.signed_set()
        result = self.verify_set()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SHA256SUMS.bundle", result.stdout)

    def test_a_leftover_per_file_bundle_fails(self):
        self.signed_set()
        (self.dist / "TaskSmack-1.2.3-win64.zip.bundle").write_text("{}", encoding="utf-8")
        result = self.verify_set()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("per-file Sigstore bundle", result.stderr)
        self.assertIn("TaskSmack-1.2.3-win64.zip.bundle", result.stderr)

    def test_a_missing_sha256sums_bundle_fails(self):
        self.signed_set()
        (self.dist / "SHA256SUMS.bundle").unlink()
        result = self.verify_set()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("dist/SHA256SUMS.bundle is missing", result.stderr)

    def test_a_missing_sbom_fails(self):
        (self.dist / "tasksmack-v1.2.3-source-sbom.spdx.json").unlink()
        self.signed_set()
        result = self.verify_set()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("source-sbom.spdx.json is missing", result.stderr)

    def test_a_file_sha256sums_does_not_list_fails(self):
        self.signed_set()
        (self.dist / "TaskSmack-1.2.3-Linux.tar.gz").write_bytes(b"late")
        result = self.verify_set()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("> TaskSmack-1.2.3-Linux.tar.gz", result.stderr)

    def test_a_file_changed_after_sha256sums_fails(self):
        self.signed_set()
        (self.dist / "TaskSmack-1.2.3-win64.zip").write_bytes(b"changed")
        result = self.verify_set()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("doesn't match its SHA256SUMS line", result.stderr)


class PublishedSha256SumsTests(unittest.TestCase):
    """release.yml verify-published-release "Verify SHA256SUMS and every asset against it" (#3145): cosign
    checks SHA256SUMS.bundle pinned to the run's own ref, then every download must match its line.
    `cosign` is a shell function that records its arguments, so the real one never runs."""

    FILES = ("TaskSmack-1.2.3-Linux.deb", "TaskSmack-1.2.3-win64.zip", "tasksmack-v1.2.3-source-sbom.spdx.json")
    COSIGN_STUB = 'cosign() { printf "%s\\n" "$@" > cosign-args.txt; return "${COSIGN_EXIT:-0}"; }\n'

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.published = self.tmp / "published"
        self.published.mkdir()
        release = WORKFLOWS / "release.yml"
        self.script = self.COSIGN_STUB + find_step(
            release, "Verify SHA256SUMS and every asset against it", job="verify-published-release"
        ).run
        lines = []
        for name in self.FILES:
            data = f"{name}\n".encode()
            (self.published / name).write_bytes(data)
            lines.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
        (self.published / "SHA256SUMS").write_text("".join(lines), encoding="utf-8", newline="\n")
        (self.published / "SHA256SUMS.bundle").write_text("{}", encoding="utf-8")
        self.summary = self.tmp / "summary.md"

    def tearDown(self):
        self._tmp.cleanup()

    def run_step(self, **env) -> subprocess.CompletedProcess:
        return run_bash(
            self.script,
            self.tmp,
            {
                "GITHUB_REF": "refs/tags/v1.2.3",
                "GITHUB_STEP_SUMMARY": posix(self.summary),
                "COSIGN_IDENTITY_REGEXP": r"^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/.+$",
                "COSIGN_OIDC_ISSUER": "https://token.actions.githubusercontent.com",
                **env,
            },
        )

    def test_a_signed_matching_download_passes_pinned_to_the_tag(self):
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = (self.tmp / "cosign-args.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(args[:3], ["verify-blob", "--bundle", "published/SHA256SUMS.bundle"])
        self.assertEqual(args[-1], "published/SHA256SUMS")
        identity = args[args.index("--certificate-identity-regexp") + 1]
        self.assertEqual(
            identity, r"^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/v1\.2\.3$"
        )
        self.assertIn("passed (3 assets", self.summary.read_text(encoding="utf-8"))

    def test_a_failed_signature_fails(self):
        result = self.run_step(COSIGN_EXIT="1")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Sigstore signature of `SHA256SUMS`: **FAILED**", self.summary.read_text(encoding="utf-8"))

    def test_a_changed_asset_fails(self):
        (self.published / "TaskSmack-1.2.3-win64.zip").write_bytes(b"tampered")
        result = self.run_step()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Assets against `SHA256SUMS`: **FAILED**", self.summary.read_text(encoding="utf-8"))

    def test_an_unlisted_or_missing_asset_fails(self):
        (self.published / "TaskSmack-1.2.3-win64.zip.bundle").write_text("{}", encoding="utf-8")
        result = self.run_step()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("> TaskSmack-1.2.3-win64.zip.bundle", result.stderr)
        (self.published / "TaskSmack-1.2.3-win64.zip.bundle").unlink()
        (self.published / "TaskSmack-1.2.3-Linux.deb").unlink()
        result = self.run_step()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("< TaskSmack-1.2.3-Linux.deb", result.stderr)


@unittest.skipUnless(PWSH, "pwsh is not on PATH")
class StageInstallerFirstPassTests(unittest.TestCase):
    """release.yml build-windows "Stage the installer's first pass" (#2302): CPack's project, the uninstaller
    it copied out and the first-pass installer go to nsis-project/, the .zip and the uninstaller to
    for-signing/, each exactly once."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.script = find_step(WORKFLOWS / "release.yml", "Stage the installer's first pass", job="build-windows").run
        self.nsis = self.tmp / "dist" / "_CPack_Packages" / "win64" / "NSIS"
        self.nsis.mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def stage(self, *, installer: str = "TaskSmack-1.2.3-win64.exe", uninstaller: bool = True):
        (self.tmp / "dist" / "TaskSmack-1.2.3-win64.zip").write_bytes(b"zip")
        (self.tmp / "dist" / installer).write_bytes(b"installer")
        (self.nsis / "project.nsi").write_text("; project\n", encoding="utf-8")
        (self.nsis / "NSIS.InstallOptions.ini").write_text("[Settings]\n", encoding="utf-8")
        if uninstaller:
            (self.nsis / "Uninstall.exe").write_bytes(b"uninstaller")
        path = self.tmp / "step.ps1"
        path.write_text("$ErrorActionPreference = 'stop'\n" + self.script + "\n", encoding="utf-8")
        return subprocess.run(
            [PWSH, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", f". '{path}'"],
            cwd=self.tmp,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout=120,
        )

    def names(self, directory: str) -> list[str]:
        return sorted(child.name for child in (self.tmp / directory).iterdir())

    def test_the_first_pass_is_staged(self):
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            self.names("nsis-project"),
            ["NSIS.InstallOptions.ini", "TaskSmack-1.2.3-win64.exe", "Uninstall.exe", "project.nsi"],
        )
        self.assertEqual(self.names("for-signing"), ["TaskSmack-1.2.3-win64.zip", "Uninstall.exe"])

    def test_no_copied_out_uninstaller_fails(self):
        result = self.stage(uninstaller=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Uninstall.exe, found 0", result.stdout + result.stderr)

    def test_an_installer_for_another_package_fails(self):
        result = self.stage(installer="TaskSmack-1.2.3-windows-arm64.exe")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("named for different packages", result.stdout + result.stderr)


class HeavyChecksCodecovUploadTests(unittest.TestCase):
    """Heavy Checks uploads coverage to Codecov after a failed test or floor step too (#2985)."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"
    CONDITION = "${{ !cancelled() && hashFiles('coverage/coverage.lcov') != '' }}"

    def test_both_uploads_run_after_a_failed_step_when_a_report_exists(self):
        for job in ("coverage", "win-coverage"):
            with self.subTest(job=job):
                step = find_step(self.WORKFLOW, "Upload coverage to Codecov", job=job)
                # Without an `if:`, the implicit success() skipped the upload once the test or
                # coverage-floor step had failed, so Codecov kept an older measurement.
                self.assertEqual(step.condition, self.CONDITION)


class SigningGalleryRegistrationTests(unittest.TestCase):
    """#3109: azure/artifact-signing-action's `Install-Module -Repository PSGallery` failed on a runner
    whose PowerShell had no PSGallery, so each signing job registers it first when it's missing."""

    WORKFLOW = WORKFLOWS / "release.yml"
    JOBS = ("sign-windows", "sign-windows-installer")
    REGISTER = "Register the PowerShell Gallery if the runner has none"
    SIGN = "Sign with Azure Artifact Signing"
    GUARD = "if (-not (Get-PSRepository -Name PSGallery -ErrorAction SilentlyContinue)) {"

    def test_each_signing_job_registers_the_gallery_just_before_the_signing_action(self):
        for job in self.JOBS:
            with self.subTest(job=job):
                names = step_names(self.WORKFLOW, job)
                self.assertIn(self.REGISTER, names)
                self.assertEqual(names.index(self.SIGN), names.index(self.REGISTER) + 1)
                sign = find_step(self.WORKFLOW, self.SIGN, job=job)
                self.assertTrue(sign.uses.startswith("azure/artifact-signing-action@"), sign.uses)

    def test_the_register_step_is_the_guarded_default_registration(self):
        for job in self.JOBS:
            with self.subTest(job=job):
                step = find_step(self.WORKFLOW, self.REGISTER, job=job)
                self.assertEqual(step.shell, "pwsh")
                self.assertEqual(step.condition, "")
                self.assertEqual(step.env, {})
                lines = step.run.splitlines()
                self.assertEqual(lines[0], self.GUARD)
                self.assertEqual(lines[1].strip(), "Register-PSRepository -Default")
                # Both jobs run the same script.
                self.assertEqual(step.run, find_step(self.WORKFLOW, self.REGISTER, job=self.JOBS[0]).run)

    def run_step(self, registered: bool) -> subprocess.CompletedProcess:
        """The step's script under pwsh, with Get-PSRepository and Register-PSRepository stubbed by
        functions (which PowerShell resolves before cmdlets) that log their calls."""
        script = find_step(self.WORKFLOW, self.REGISTER, job=self.JOBS[0]).run
        stubs = (
            f"$script:registered = ${str(registered).lower()}\n"
            "function Get-PSRepository {\n"
            "  param([string]$Name, $ErrorAction)\n"
            "  Write-Host \"stub: Get-PSRepository $Name\"\n"
            "  if ($script:registered) { return [pscustomobject]@{ Name = 'PSGallery' } }\n"
            "  if ($ErrorAction -eq 'Stop') { throw 'no PSGallery' }\n"
            "}\n"
            "function Register-PSRepository {\n"
            "  param([switch]$Default)\n"
            "  Write-Host \"stub: Register-PSRepository -Default:$Default\"\n"
            "  $script:registered = $true\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "step.ps1"
            path.write_text(stubs + script + "\n", encoding="utf-8")
            return subprocess.run(
                # GitHub's `shell: pwsh` wrapper: -command ". '{0}'" with $ErrorActionPreference = 'stop'.
                [PWSH, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
                 f"$ErrorActionPreference = 'stop'; . '{path}'"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                check=False,
                timeout=60,
            )

    @unittest.skipUnless(PWSH, "needs pwsh")
    def test_a_runner_without_the_gallery_registers_it(self):
        result = self.run_step(registered=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("stub: Register-PSRepository -Default:True", result.stdout)
        self.assertIn("PSGallery was not registered: registered the default repository.", result.stdout)

    @unittest.skipUnless(PWSH, "needs pwsh")
    def test_a_runner_with_the_gallery_registers_nothing(self):
        result = self.run_step(registered=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Register-PSRepository", result.stdout)
        self.assertIn("PSGallery is already registered: nothing to do.", result.stdout)


# Stands in for the release-notes job's gh calls (#3144). `release view` answers isDraft from the next
# line of $FIXTURES/states (missing, draft or published; the last line repeats) and body/assets from
# $FIXTURES/release.json; `release edit` copies the notes file to $FIXTURES/edited.md. Every call is
# logged to $FIXTURES/calls.log.
RELEASE_GH_STUB = r"""#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FIXTURES/calls.log"
case "$1 $2" in
  "release view")
    if [[ "$*" == *"--json isDraft"* ]]; then
      count=$(grep -c 'isDraft' "$FIXTURES/calls.log")
      state=$(sed -n "${count}p" "$FIXTURES/states")
      [[ -n "$state" ]] || state=$(tail -n1 "$FIXTURES/states")
      case "$state" in
        missing) echo "release not found" >&2; exit 1 ;;
        error) echo "HTTP 502: Bad Gateway" >&2; exit 1 ;;
        draft) echo true ;;
        published) echo false ;;
      esac
    else
      cat "$FIXTURES/release.json"
    fi ;;
  "release edit")
    while [[ $# -gt 0 ]]; do
      if [[ "$1" == --notes-file ]]; then cp "$2" "$FIXTURES/edited.md"; fi
      shift
    done ;;
  *) echo "gh stub: unexpected command $*" >&2; exit 2 ;;
esac
"""


class ChangelogReleaseNotesTests(unittest.TestCase):
    """#3144: v0.9.1 shipped with release.yml's "Release notes follow." placeholder until its notes
    were applied by hand. changelog.yml's release-notes job waits for the release to be published,
    then replaces the placeholder with tools/compose-release-notes.py's notes, and leaves notes
    someone wrote alone."""

    WORKFLOW = WORKFLOWS / "changelog.yml"
    JOB = "release-notes"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.fixtures = self.tmp / "fixtures"
        self.fixtures.mkdir()
        (self.fixtures / "calls.log").write_text("", encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    def run_step(self, name: str, env: dict[str, str]) -> subprocess.CompletedProcess:
        step = find_step(self.WORKFLOW, name, job=self.JOB)
        stub = self.tmp / "gh"
        stub.write_text(RELEASE_GH_STUB, encoding="utf-8", newline="\n")
        script = (
            # GitHub's bash: -e -o pipefail. gh and python3 are shell functions, so the real gh can never
            # answer and python3 is this interpreter (Git Bash's may be the Store alias), and the step
            # runs from the checkout's root as on the runner.
            "set -eo pipefail\n"
            f'gh() {{ "$BASH" "{posix(stub)}" "$@"; }}\n'
            f'python3() {{ "{posix(Path(sys.executable))}" "$@"; }}\n'
            "sleep() { :; }\n"
            f'cd "{posix(REPO)}"\n' + step.run
        )
        values = {"runner.temp": posix(self.tmp), "github.token": "not-a-token"}
        step_env = {key: substitute(value, values).strip("'") for key, value in step.env.items()}
        return run_bash(
            script,
            self.tmp,
            {**step_env, **env, "FIXTURES": posix(self.fixtures), "TAG_NAME": "v0.10.0", "RUNNER_TEMP": posix(self.tmp)},
        )

    def calls(self) -> list[str]:
        return (self.fixtures / "calls.log").read_text(encoding="utf-8").splitlines()

    def wait(self, *states: str, minutes: str = "5") -> subprocess.CompletedProcess:
        (self.fixtures / "states").write_text("\n".join(states) + "\n", encoding="utf-8")
        return self.run_step("Wait for the release to be published", {"WAIT_MINUTES": minutes, "POLL_SECONDS": "0"})

    def test_the_job_waits_then_composes_with_least_privilege(self):
        self.assertEqual(
            step_names(self.WORKFLOW, self.JOB),
            ["Generate the tag's changelog section", "Wait for the release to be published",
             "Replace the placeholder notes"],
        )
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text[text.index("\n  release-notes:\n"):]
        block = job[job.index("    permissions:\n"):job.index("    steps:\n")].splitlines()[1:]
        self.assertEqual([line.split("#")[0].strip() for line in block], ["contents: write", "pull-requests: read"])
        self.assertIn("persist-credentials: false", job)
        # The changelog job's git-cliff and range, without the file header the composer would drop anyway.
        cliff = find_step(self.WORKFLOW, "Generate the tag's changelog section", job=self.JOB)
        self.assertEqual(cliff.uses, "orhun/git-cliff-action@a9a95522b26fe6403f7bb24031f21fb573d0f5ff # v4")
        self.assertIn("args: --verbose --strip header ${{ needs.targets.outputs.previous }}..${{ github.ref_name }}", job)

    def test_a_draft_or_missing_release_is_waited_for(self):
        result = self.wait("missing", "error", "draft", "draft", "published")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("is not created yet; checking again", result.stdout)
        self.assertIn("is unreadable (HTTP 502: Bad Gateway); checking again", result.stdout)
        self.assertIn("is still a draft; checking again", result.stdout)
        self.assertIn("The release for 'v0.10.0' is published.", result.stdout)
        self.assertEqual(len(self.calls()), 5)

    def test_a_release_never_published_fails_the_wait(self):
        result = self.wait("draft", minutes="0")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("::error title=Release not published::The release for 'v0.10.0' is still a draft after 0 minutes", result.stdout)

    def write_release(self, body: str) -> None:
        section = "## [0.10.0] - 2026-11-20\n### Fixed\n\n- A fix\n"
        (self.tmp / "changelog-section.md").write_text(section, encoding="utf-8")
        assets = [{"name": name} for name in ("TaskSmack-0.10.0-win64.zip", "SHA256SUMS", "SHA256SUMS.bundle")]
        (self.fixtures / "release.json").write_text(json.dumps({"body": body, "assets": assets}), encoding="utf-8")

    def test_the_placeholder_is_replaced(self):
        verify = "## Verifying downloads\n\nCheck `SHA256SUMS`."
        self.write_release("Release notes follow.\n\n" + verify + "\n")
        result = self.run_step("Replace the placeholder notes", {})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Replaced the placeholder notes of the 'v0.10.0' release.", result.stdout)
        self.assertIn("release edit v0.10.0 --notes-file", self.calls()[-1])
        notes = (self.fixtures / "edited.md").read_text(encoding="utf-8")
        self.assertTrue(notes.startswith("# TaskSmack v0.10.0\n\n## Which file should I download?\n"))
        self.assertIn("## Changes\n\n#### Fixed\n\n- A fix\n", notes)
        self.assertIn("`SHA256SUMS`, `SHA256SUMS.bundle`, `*-build-inputs.json`", notes)
        self.assertTrue(notes.endswith(verify + "\n"))

    def test_hand_written_notes_are_never_edited(self):
        self.write_release("# TaskSmack v0.10.0\n\nThe maintainer's notes.\n")
        result = self.run_step("Replace the placeholder notes", {})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("release notes already set; left as is", result.stdout)
        self.assertFalse(any(call.startswith("release edit") for call in self.calls()), self.calls())
        self.assertFalse((self.fixtures / "edited.md").exists())


SETUP_LLVM = REPO / ".github" / "actions" / "setup-llvm"

OS_RELEASE = 'ID=ubuntu\nVERSION_ID="26.04"\nPRETTY_NAME="Ubuntu 26.04 LTS"\n'


class SetupLlvmCacheKeyTests(unittest.TestCase):
    """#3635: every setup-llvm caller gets a weekly LLVM package cache key with a restore-key to the newest
    earlier week; a pinned release (#3393) keeps its exact key and gets no restore-key."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        (self.tmp / "os-release").write_text(OS_RELEASE, encoding="utf-8", newline="\n")
        step = find_step(SETUP_LLVM / "action.yml", "LLVM package cache key")
        self.assertEqual(step.condition, "", "every caller gets the cache, not only expect-release")
        # The runner's /etc/os-release, from a fixture, so the image part doesn't depend on the test machine.
        self.script = step.run.replace("/etc/os-release", posix(self.tmp / "os-release"))

    def tearDown(self):
        self._tmp.cleanup()

    def outputs(self, **inputs: str) -> tuple[subprocess.CompletedProcess, dict[str, str]]:
        out = self.tmp / "out.txt"
        out.write_text("", encoding="utf-8")
        env = {
            "EXPECT_RELEASE": "",
            "INSTALL_CLANG_FORMAT": "false",
            "INSTALL_CLANG_TIDY": "false",
            "LLVM_VERSION": "23",
            "RUNNER_ARCH": "X64",
            "RUNNER_TEMP": posix(self.tmp),
            "GITHUB_OUTPUT": posix(out),
            **inputs,
        }
        result = run_bash(self.script, self.tmp, env)
        values = dict(line.split("=", 1) for line in out.read_text(encoding="utf-8").splitlines() if line)
        return result, values

    def test_without_a_release_the_key_ends_in_the_week_and_falls_back_to_earlier_weeks(self):
        result, out = self.outputs()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(out["key"], r"^Linux-llvm-debs-23-ubuntu26\.04-X64-[0-9a-f]{12}-v1-\d{4}-W\d{2}$")
        self.assertEqual(out["restore-keys"], re.sub(r"\d{4}-W\d{2}$", "", out["key"]))
        self.assertTrue(out["path"].endswith("/llvm-debs"))

    def test_a_release_keeps_its_exact_key_and_no_restore_key(self):
        result, out = self.outputs(EXPECT_RELEASE="23.1.6")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The key of the entries release.yml has saved since #3393, unchanged.
        set_id = hashlib.sha256(b"format=false tidy=false").hexdigest()[:12]
        self.assertEqual(out["key"], f"Linux-llvm-debs-23.1.6-ubuntu26.04-X64-{set_id}-v1")
        self.assertEqual(out["restore-keys"], "")

    def test_each_package_set_and_architecture_has_its_own_entry(self):
        keys = set()
        for inputs in (
            {},
            {"INSTALL_CLANG_TIDY": "true"},
            {"INSTALL_CLANG_FORMAT": "true", "INSTALL_CLANG_TIDY": "true"},
            {"RUNNER_ARCH": "ARM64"},
        ):
            result, out = self.outputs(**inputs)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            keys.add(out["restore-keys"])
        self.assertEqual(len(keys), 4, keys)

    def test_a_bad_version_or_release_fails(self):
        for inputs in ({"LLVM_VERSION": "23.1"}, {"EXPECT_RELEASE": "23.1"}, {"EXPECT_RELEASE": "22.1.8"}):
            with self.subTest(inputs=inputs):
                result, _ = self.outputs(**inputs)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("::error::", result.stdout)

    def test_the_restore_passes_the_restore_key_and_the_install_sees_the_matched_key(self):
        restore = find_step(SETUP_LLVM / "action.yml", "Restore the LLVM package cache")
        self.assertEqual(restore.condition, "")
        text = (SETUP_LLVM / "action.yml").read_text(encoding="utf-8")
        self.assertIn("restore-keys: ${{ steps.package-cache-key.outputs.restore-keys }}", text)
        install = find_step(SETUP_LLVM / "action.yml", "Install LLVM ${{ inputs.llvm-version }}")
        self.assertEqual(install.env["CACHE_MATCHED_KEY"], "${{ steps.package-cache.outputs.cache-matched-key }}")

    def test_the_action_saves_only_without_a_release_and_only_on_main_and_release_branches(self):
        save = find_step(SETUP_LLVM / "action.yml", "Save the LLVM package cache (main and release/* only)")
        self.assertTrue(save.uses.startswith("actions/cache/save@"), save.uses)
        # Its `if:` is a folded block, which workflow_steps doesn't read: the text between the name and `uses:`.
        text = (SETUP_LLVM / "action.yml").read_text(encoding="utf-8")
        after = text[text.index("- name: Save the LLVM package cache (main and release/* only)") :]
        condition = " ".join(after[: after.index("uses:")].split())
        for clause in (
            "inputs.expect-release == ''",
            "steps.install.outputs.staged == 'true'",
            "github.ref == 'refs/heads/main'",
            "startsWith(github.ref, 'refs/heads/release/')",
        ):
            self.assertIn(clause, condition)


# Stubs for install-llvm.sh, as shell functions ahead of the script, so no real command can answer whichever
# bash runs it (Git Bash rebuilds PATH from the Windows one). sudo runs `timeout ... <command>` as <command>
# and drops everything else (tee, sed, rm, install), so nothing touches the machine. apt-get, curl, dpkg,
# dpkg-query, sha256sum and sleep answer from the environment and log each call to $STUB_LOG. curl tells the
# Ubuntu mirror probe (#3637), which always answers, from apt.llvm.org, which CURL_EXIT can take down.
LLVM_STUBS = r"""
sudo() {
  echo "sudo $*" >> "$STUB_LOG"
  if [[ "$1" == timeout ]]; then
    shift
    while [[ "$1" == --* ]]; do shift; done
    shift
    "$@"
    return
  fi
  if [[ "$1" == tee ]]; then cat > /dev/null; fi
  return 0
}
apt-get() {
  echo "apt-get $*" >> "$STUB_LOG"
  if [[ "$1" == download ]]; then
    shift
    local pin
    for pin in "$@"; do
      : > "${pin%%=*}_$(printf '%s' "${pin#*=}" | sed 's/:/%3a/g')_amd64.deb"
    done
  fi
  if [[ "$1" == update ]]; then return "${APT_UPDATE_EXIT:-0}"; fi
  return 0
}
curl() {
  echo "curl $*" >> "$STUB_LOG"
  if [[ "$*" != *apt.llvm.org* ]]; then return 0; fi
  if [[ "${CURL_EXIT:-0}" != 0 ]]; then return "$CURL_EXIT"; fi
  local out=''
  while (( $# )); do
    if [[ "$1" == --output ]]; then out=$2; shift; fi
    shift
  done
  printf '#!/usr/bin/env bash\necho "llvm.sh $*" >> "$STUB_LOG"\nexit "${LLVM_SH_EXIT:-0}"\n' > "$out"
}
dpkg() { echo amd64; }
dpkg-query() {
  if [[ "$2" == '-f=${Version}' ]]; then echo "$CLANG_VERSION"; return 0; fi
  cat "$DPKG_LIST"
}
sha256sum() { cat > /dev/null; return "${SHA_EXIT:-0}"; }
sleep() { :; }
"""

NEW_BUILD = "1:23.1.6~++20261110+abc-1~exp1~20261110.1"
OLD_BUILD = "1:23.1.6~++20261103+def-1~exp1~20261103.1"
LLVM_NAMED = ["clang-23", "lldb-23", "lld-23", "clangd-23", "libc++-23-dev", "libc++abi-23-dev"]


class SetupLlvmInstallTests(unittest.TestCase):
    """#3635: install-llvm.sh installs from the exact cache entry without apt.llvm.org, refreshes an older
    entry from apt.llvm.org and falls back to it when apt.llvm.org is down, and still fails a cold miss."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cache = self.tmp / "llvm-debs"
        self.work = self.tmp / "work"
        self.work.mkdir()
        self.log = self.tmp / "calls.log"
        self.out = self.tmp / "out.txt"
        dpkg_list = self.tmp / "dpkg.txt"
        installed = [f"ii {pkg}={NEW_BUILD}" for pkg in LLVM_NAMED + ["libllvm23", "libclang-cpp23"]]
        dpkg_list.write_text("\n".join(installed + ["ii libc6=2.42-0ubuntu1"]) + "\n", encoding="utf-8")
        self.dpkg_list = dpkg_list

    def tearDown(self):
        self._tmp.cleanup()

    def fill_cache(self, build: str, packages: list[str] | None = None) -> None:
        self.cache.mkdir(exist_ok=True)
        for pkg in packages if packages is not None else LLVM_NAMED + ["libllvm23"]:
            (self.cache / f"{pkg}_{build.replace(':', '%3a')}_amd64.deb").write_text("", encoding="utf-8")

    def run_install(self, **env: str) -> tuple[subprocess.CompletedProcess, dict[str, str], list[str]]:
        self.log.write_text("", encoding="utf-8")
        self.out.write_text("", encoding="utf-8")
        script = LLVM_STUBS + (SETUP_LLVM / "install-llvm.sh").read_text(encoding="utf-8")
        # The runner's /etc/os-release (the mirror probe reads its codename), from a fixture.
        os_release = self.tmp / "os-release"
        os_release.write_text(OS_RELEASE + "VERSION_CODENAME=resolute\n", encoding="utf-8", newline="\n")
        script = script.replace("/etc/os-release", posix(os_release))
        full_env = {
            "STUB_LOG": posix(self.log),
            "GITHUB_OUTPUT": posix(self.out),
            "DPKG_LIST": posix(self.dpkg_list),
            "CLANG_VERSION": NEW_BUILD,
            "LLVM_VERSION": "23",
            "EXPECT_RELEASE": "",
            "CACHE_DIR": posix(self.cache),
            "CACHE_RESTORED": "false",
            "CACHE_MATCHED_KEY": "",
            "INSTALL_NINJA": "true",
            "INSTALL_CCACHE": "true",
            "INSTALL_CLANG_FORMAT": "false",
            "INSTALL_CLANG_TIDY": "false",
            **env,
        }
        result = run_bash(script, self.work, full_env)
        outputs = dict(line.split("=", 1) for line in self.out.read_text(encoding="utf-8").splitlines() if line)
        return result, outputs, self.log.read_text(encoding="utf-8").splitlines()

    @staticmethod
    def llvm_sh_tries(calls: list[str]) -> int:
        return sum(1 for call in calls if call.startswith("llvm.sh "))

    def staged(self) -> list[str]:
        return sorted(path.name for path in self.cache.glob("*.deb"))

    def test_an_exact_hit_installs_the_cached_debs_without_apt_llvm_org(self):
        self.fill_cache(NEW_BUILD)
        result, out, calls = self.run_install(CACHE_RESTORED="true", CACHE_MATCHED_KEY="k-2026-W46")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out, {"cache-hit": "true", "staged": "false"})
        self.assertFalse([call for call in calls if "apt.llvm.org" in call or "llvm.sh" in call], calls)
        install = [call for call in calls if call.startswith("apt-get install")]
        self.assertEqual(len(install), 1, calls)
        self.assertIn("clang-23_", install[0])
        self.assertIn("ninja-build", install[0])

    def test_an_exact_hit_missing_a_package_goes_to_apt_llvm_org(self):
        self.fill_cache(NEW_BUILD)
        result, out, calls = self.run_install(CACHE_RESTORED="true", INSTALL_CLANG_TIDY="true")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("The LLVM package cache has no clang-tidy-23", result.stdout)
        self.assertEqual(out["cache-hit"], "false")
        self.assertEqual(self.llvm_sh_tries(calls), 1, calls)

    def test_a_miss_installs_from_apt_llvm_org_and_stages_clangs_build(self):
        result, out, calls = self.run_install()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out, {"cache-hit": "false", "staged": "true"})
        self.assertEqual(self.llvm_sh_tries(calls), 1, calls)
        # clang's own build: the named packages and the libraries they pulled in, never libc6.
        build = NEW_BUILD.replace(":", "%3a")
        expected = [f"{pkg}_{build}_amd64.deb" for pkg in LLVM_NAMED + ["libclang-cpp23", "libllvm23"]]
        self.assertEqual(self.staged(), sorted(expected))

    def test_a_cold_miss_still_fails_when_apt_llvm_org_is_down(self):
        result, out, calls = self.run_install(LLVM_SH_EXIT="124")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.llvm_sh_tries(calls), 4, calls)
        self.assertIn("::error::", result.stdout)
        self.assertNotIn("staged", out)

    def test_an_older_entry_is_refreshed_from_apt_llvm_org(self):
        self.fill_cache(OLD_BUILD)
        result, out, calls = self.run_install(CACHE_MATCHED_KEY="k-2026-W45")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out, {"cache-hit": "false", "staged": "true"})
        self.assertIn("refreshing it from apt.llvm.org", result.stdout)
        self.assertTrue(self.staged() and all(NEW_BUILD.replace(":", "%3a") in name for name in self.staged()))

    def test_an_older_entry_stands_in_when_apt_llvm_org_is_down(self):
        for failure in ({"LLVM_SH_EXIT": "124"}, {"CURL_EXIT": "28"}):
            with self.subTest(failure=failure):
                self.fill_cache(OLD_BUILD)
                result, out, calls = self.run_install(CACHE_MATCHED_KEY="k-2026-W45", **failure)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(out, {"cache-hit": "false", "staged": "false"})
                self.assertLessEqual(self.llvm_sh_tries(calls), 2, calls)
                self.assertIn("::warning::apt.llvm.org failed", result.stdout)
                # A job that passes on the older entry shows no error annotation.
                self.assertNotIn("::error::", result.stdout)
                install = [call for call in calls if call.startswith("apt-get install")][-1]
                self.assertIn("--allow-downgrades", install)
                self.assertIn(f"clang-23_{OLD_BUILD.replace(':', '%3a')}_amd64.deb", install)
                # Not saved again under this week's key.
                self.assertTrue(all(OLD_BUILD.replace(":", "%3a") in name for name in self.staged()))

    def test_a_changed_installer_fails_even_with_an_older_entry(self):
        self.fill_cache(OLD_BUILD)
        result, _, calls = self.run_install(CACHE_MATCHED_KEY="k-2026-W45", SHA_EXIT="1")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertEqual(self.llvm_sh_tries(calls), 0, calls)

    def test_a_release_never_uses_an_older_entry(self):
        # #3393: a release restores only its exact key; a matched key alone is no hit and no fallback.
        self.fill_cache(OLD_BUILD)
        result, out, calls = self.run_install(EXPECT_RELEASE="23.1.6", CACHE_MATCHED_KEY="k", LLVM_SH_EXIT="124")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.llvm_sh_tries(calls), 4, calls)

    def test_a_release_miss_stages_every_package_of_the_release(self):
        result, out, _ = self.run_install(EXPECT_RELEASE="23.1.6")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out["staged"], "true")
        self.assertEqual(len(self.staged()), len(LLVM_NAMED) + 2)
        self.assertIn("Staged 8 LLVM 23.1.6 .debs", result.stdout)


def _is_setup_llvm(test: unittest.TestCase) -> bool:
    return type(test).__name__.startswith("SetupLlvm")


def _filtered(suite: unittest.TestSuite, keep) -> unittest.TestSuite:
    out = unittest.TestSuite()
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            out.addTests(_filtered(item, keep))
        elif keep(item):
            out.addTest(item)
    return out


if __name__ == "__main__":
    # #3643: the setup-llvm cases (#3635) spawn bash about forty times, so ctest runs them as their own entry,
    # SetupLlvmInstallScript (--only-setup-llvm), and WorkflowRunStepsScript runs the rest (--skip-setup-llvm).
    # With neither flag every case runs, as before.
    flags = {"--only-setup-llvm", "--skip-setup-llvm"}
    chosen = [arg for arg in sys.argv[1:] if arg in flags]
    if chosen:
        sys.argv = [arg for arg in sys.argv if arg not in flags]
        only = chosen[0] == "--only-setup-llvm"
        loaded = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
        keep = _is_setup_llvm if only else (lambda test: not _is_setup_llvm(test))
        result = unittest.TextTestRunner(verbosity=1).run(_filtered(loaded, keep))
        sys.exit(0 if result.wasSuccessful() else 1)
    unittest.main()
