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
- release.yml rehearse-winget (#3822): the dry run builds and checks the previous release's WinGet manifests
  with no secret, no environment and no `submit`, so the only job that can read WINGET_TOKEN is publish-winget,
  which a tag push alone starts. Read from the workflow's text; nothing runs.
- .github/actions/setup-llvm "LLVM package cache key" and install-llvm.sh: every caller gets a weekly LLVM
  package cache key with a restore-key, a pinned release keeps its exact key (#3393), and the script
  installs an exact hit without apt.llvm.org, refreshes an older entry and falls back to it when
  apt.llvm.org is down, and still fails a cold miss (#3635). sudo, apt-get, curl and dpkg are stubs.
- ci.yml include-analysis (#4481): the IWYU build gets llvm-23-dev and libclang-23-dev from setup-llvm's pinned
  apt.llvm.org source (install-iwyu-build-deps), never from a bare apt-get against Ubuntu's own archive, which
  has no LLVM 23; setup-llvm keeps every other caller's cache key and adds the two packages to this one's.
- Merge queue (#3818): every check the "Main Builds" ruleset requires (REQUIRED_CHECKS, kept in one place)
  comes from a workflow with the `merge_group` trigger, and none is skipped on a merge group. Each producing
  workflow's jobs are simulated from their own `if:` expressions (a small expression evaluator), with the detect
  jobs' steps run for real, and the CI Success and OSV gates' steps run on the results. Concurrency groups never
  join two queue entries, no push or workflow_run trigger fires for a gh-readonly-queue branch, a merge group
  saves no clang-tidy result cache, and pre-commit's version guard reads the queue's base branch.
- release-dashboard.yml generate "Wait until the published data is five minutes old" (#4604, #4620): a run
  that has something to add waits out the rest of the five minutes, and until 15 seconds after it was
  created so a merge's second event is in its data, whichever is longer and no more; every trigger but a
  dispatch and a pull request waits; the job's timeout outlasts the wait; and a manual run cancels the run in
  its way where every other event queues. `date` and `sleep` are stubs.
- release-dashboard.yml generate "Stop here when this run has nothing to add" (#4620): a run stops before it
  waits or reads when the token's REST calls or GraphQL points are gone (a warning, not a failure for every
  event), and when the published figures were read at least 10 seconds after the run was created (the second
  run of a merge, and one that waited behind another); nothing is generated or published after a stop; and
  only the events worth a run start one (no labels, no edits, no Main Health, and since #4700 no `opened`:
  `milestoned` covers a new issue), with two schedule lines five minutes apart. `date +%s` and `gh` are
  stubs; jq and `date -d` are real, and the cases are skipped without jq.
- release-dashboard.yml (#4726): a run the spent budget stops hands on when the budget returns, once for each
  hour (not when the published data names that hour already, and not with nothing published), and "Tell the page
  when the budget returns" writes the last data again with that time. The generator and the check step's own
  script run for real on the committed fixtures.
- release-dashboard.yml (#4717): the wait asks about the budget again after waiting and stops the run with a
  warning when it's gone; generate's "Pack the data for the publish job" and publish's "Unpack the data" hand
  the JSON over as a job output with no artifact (only a run that can't publish keeps one, for its reviewer), a
  string with a line break in it is refused and never runs as shell, and publish refuses anything but a flat
  set of JSON files; and "Replace the branch's one commit" tries its push three times. tar, gzip, base64 and
  jq are real; `git`, `timeout`, `sleep` and `gh` are shell functions.
- main-health.yml "Open, update or close the tracking issue" (#4700): a run that opened or closed a tracking
  issue dispatches release-dashboard.yml once, however many branches changed, and does so on exit, so also
  when a later branch then fails the step; a run that changed nothing or only rewrote an open issue
  dispatches nothing; and a dispatch that fails is a warning, not a failed check. In the dashboard that run
  queues (only a person's cancels the run in progress) and stops if the data already has main's new state.
  `gh` and `sleep` are shell functions that log each call; the step's own update_issue(), trap and loop run.

Each step is read from the workflow file itself (tests/tools/workflow_steps.py), its `${{ }}`
expressions replaced by fixed values, and run with bash (pwsh for the pwsh step). Needs bash; the
Main Health and CodeQL cases also need jq, and the reproducibility cases pwsh, and are skipped
without them.
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile
from dataclasses import dataclass, field
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


# The steps run on ubuntu runners only, and tools/bench-across-launches.py run starts each launch, ./tools/bench.sh,
# as a program, which Windows can't do with a shell script.
@unittest.skipIf(os.name == "nt", "the benchmark gate's steps run on Linux")
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
        # Files bash reads are written with LF on every platform (here, the counter): a
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

    def test_a_launch_past_the_deadline_is_stopped_and_the_flag_stands(self):
        # #3508: the estimate can be short, so each launch runs under timeout with what's left of the budget.
        # 4 s: the plan's estimate for one benchmark with no timed repetitions is 1 s a launch, 4 s in all.
        first = {**self.BASELINE, "BM_Big": 1600.0}
        launches = [{"BM_Big": 1000.0}] * 4
        # tools/bench-across-launches.py run stops the launch at the deadline: its whole process group, the stub's
        # sleep included. The deadline between launches is tested there (test_bench_across_launches.py).
        started = time.monotonic()
        check_exit, remeasure, outputs = self.gate(first, launches, {
            "MAX_SECONDS": "4", "STUB_SLOW_LAUNCH": "2", "STUB_SLOW_SECONDS": "30", "JOB_START_EPOCH": str(int(time.time()))})
        self.assertLess(time.monotonic() - started, 25, "the slow launch was killed, not waited out")
        self.assertEqual(remeasure.returncode, 0, remeasure.stdout + remeasure.stderr)
        self.assertEqual(outputs["exit_code"], "1")
        self.assertTrue(self.fails(check_exit, outputs["exit_code"]))
        report = (self.work / "benchmark-across-launches-output.txt").read_text(encoding="utf-8")
        self.assertIn("Out of time: launch 2 of 5 stopped after 4s, past the 4s budget. The flags stand", report)
        self.assertFalse((self.work / "perf-data" / "launches" / "2").exists(), "the killed launch's partial output")
        self.assertFalse((self.work / "perf-data" / "benchmark-across-launches.json").exists())

    def test_a_failed_launch_fails_the_step(self):
        first = {**self.BASELINE, "BM_Big": 1600.0}
        # Launch 3 doesn't measure it: the combine refuses, and the step fails.
        launches = [{"BM_Big": 1000.0}, {}, {"BM_Big": 1000.0}, {"BM_Big": 1000.0}]
        check_exit, remeasure, outputs = self.gate(first, launches)
        self.assertNotEqual(remeasure.returncode, 0, remeasure.stdout)
        self.assertNotIn("exit_code", outputs)
        self.assertIn("no usable median in launch 3", remeasure.stdout + remeasure.stderr)


# The steps run on ubuntu runners only, and tools/bench-across-launches.py run starts each launch, ./tools/bench.sh,
# as a program, which Windows can't do with a shell script.
@unittest.skipIf(os.name == "nt", "the benchmark gate's steps run on Linux")
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


class HeavyChecksCoverageFloorTests(unittest.TestCase):
    """#3842: the Windows line-coverage floor is 93% and a drop below it fails the job, as Linux's does."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"

    def test_windows_floor_is_93_and_below_it_fails_the_step(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("WINDOWS_COVERAGE_THRESHOLD: '93'", text)
        step = find_step(self.WORKFLOW, "Check coverage threshold", job="win-coverage")
        self.assertIn("::error title=Windows coverage::", step.run)
        self.assertNotIn("::warning title=Windows coverage::Line coverage", step.run)
        self.assertIn("exit 1", step.run)


class Arm64LabelerTests(unittest.TestCase):
    """#3820: the PR labeler adds `ci:arm64` by path, and arm64.yml runs a PR only on that label."""

    def test_labeler_maps_build_system_paths_to_the_arm64_label(self):
        text = (WORKFLOWS.parent / "labeler.yml").read_text(encoding="utf-8")
        self.assertIn('"ci:arm64":', text)
        block = text.split('"ci:arm64":', 1)[1].split("\n\"", 1)[0]
        for glob in (".github/actions/**", "cmake/**", "CMakeLists.txt", "CMakePresets.json", "vcpkg*.json"):
            self.assertIn(f'"{glob}"', block)

    def test_arm64_jobs_run_a_pull_request_only_with_the_label(self):
        text = (WORKFLOWS / "arm64.yml").read_text(encoding="utf-8")
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'ci:arm64')", text)

    # #3930: the labeler's token starts no workflow, so arm64.yml's detect job reads the same globs itself.
    DECIDE = "Decide whether ARM64 runs"

    def test_both_build_jobs_wait_for_the_detect_job_and_run_on_its_output(self):
        text = (WORKFLOWS / "arm64.yml").read_text(encoding="utf-8")
        for job in ("build-linux-arm64", "build-windows-arm64"):
            with self.subTest(job=job):
                block = text.split(f"\n  {job}:\n", 1)[1].split("\n  build-", 1)[0]
                self.assertIn("    needs: detect\n", block)
                self.assertIn("    if: needs.detect.outputs.run == 'true'\n", block)

    def test_detect_reads_the_labelers_file_through_the_tool(self):
        step = find_step(WORKFLOWS / "arm64.yml", self.DECIDE, job="detect")
        self.assertIn(
            "python3 -I tools/arm64-paths.py --labeler .github/labeler.yml --base 'HEAD^1' --head HEAD", step.run
        )
        checkout = find_step(WORKFLOWS / "arm64.yml", "Check out the pull request's merge commit", job="detect")
        self.assertTrue(checkout.uses.startswith("actions/checkout@"), checkout.uses)

    def decide(
        self, event: str, action: str = "", label: str = "", has_label: bool = False, tool: str = "echo true"
    ) -> tuple[subprocess.CompletedProcess, str]:
        """The detect step under bash, with a stub python3 that runs `tool` and a GITHUB_OUTPUT file."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "bin").mkdir()
            stub = directory / "bin" / "python3"
            stub.write_text(f"#!/usr/bin/env bash\n{tool}\n", encoding="utf-8", newline="\n")
            stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
            output = directory / "github_output"
            summary = directory / "github_summary"
            output.write_text("", encoding="utf-8")
            summary.write_text("", encoding="utf-8")
            step = find_step(WORKFLOWS / "arm64.yml", self.DECIDE, job="detect")
            result = run_bash(
                step.run,
                directory,
                {
                    "PATH": f"{posix(directory / 'bin')}{os.pathsep}{os.environ['PATH']}",
                    "EVENT_NAME": event,
                    "EVENT_ACTION": action,
                    "LABEL_NAME": label,
                    "HAS_LABEL": "true" if has_label else "false",
                    "GITHUB_OUTPUT": posix(output),
                    "GITHUB_STEP_SUMMARY": posix(summary),
                },
            )
            return result, output.read_text(encoding="utf-8").strip()

    def test_a_push_the_schedule_and_a_dispatch_always_run(self):
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                result, out = self.decide(event, tool="echo false")
                self.assertEqual((result.returncode, out), (0, "run=true"), result.stderr)

    def test_a_pull_request_with_the_label_runs_without_looking_at_paths(self):
        result, out = self.decide("pull_request", "synchronize", has_label=True, tool="echo false")
        self.assertEqual((result.returncode, out), (0, "run=true"), result.stderr)

    def test_a_pull_request_without_the_label_runs_when_the_paths_match(self):
        result, out = self.decide("pull_request", "synchronize", tool="echo true")
        self.assertEqual((result.returncode, out), (0, "run=true"), result.stderr)

    def test_a_pull_request_without_the_label_or_matching_paths_does_not_run(self):
        result, out = self.decide("pull_request", "opened", tool="echo false")
        self.assertEqual((result.returncode, out), (0, "run=false"), result.stderr)

    def test_adding_another_label_runs_nothing(self):
        result, out = self.decide("pull_request", "labeled", label="documentation", has_label=True, tool="echo true")
        self.assertEqual((result.returncode, out), (0, "run=false"), result.stderr)

    def test_adding_the_arm64_label_runs(self):
        result, out = self.decide("pull_request", "labeled", label="ci:arm64", has_label=True, tool="echo false")
        self.assertEqual((result.returncode, out), (0, "run=true"), result.stderr)

    def test_a_failed_path_check_runs_arm64(self):
        result, out = self.decide("pull_request", "synchronize", tool="exit 2")
        self.assertEqual((result.returncode, out), (0, "run=true"), result.stderr)
        self.assertIn("::warning", result.stdout)


class RealSystemOnArm64Tests(unittest.TestCase):
    """#3823: the Windows real-system tests run on the ARM64 leg too, with the same guards as on x64."""

    WORKFLOW = WORKFLOWS / "reusable-build-test.yml"
    STEP = "Real-system tests (Windows)"

    def test_the_step_has_no_architecture_guard(self):
        step = find_step(self.WORKFLOW, self.STEP)
        self.assertNotIn("runner.arch", step.condition)
        self.assertIn("runner.os == 'Windows'", step.condition)
        self.assertIn("steps.build-windows.outcome == 'success'", step.condition)

    def test_the_step_never_starts_after_a_cancel_or_without_a_good_build(self):
        # #3906: always() is true on a cancelled run, which started a 10-minute machine-changing run on a runner
        # that was shutting down.
        step = find_step(self.WORKFLOW, self.STEP)
        self.assertNotIn("always()", step.condition)
        self.assertIn("!cancelled()", step.condition)
        self.assertIn("steps.build-windows.outcome == 'success'", step.condition)
        self.assertIn("inputs.build", step.condition)

    def test_the_plain_windows_ctest_step_still_skips_arm64_for_its_own_step(self):
        # ARM64's ctest runs in "Test (Windows ARM64)", which this change leaves alone.
        self.assertIn("runner.arch != 'ARM64'", find_step(self.WORKFLOW, "Test (Windows)").condition)
        self.assertIn("runner.arch == 'ARM64'", find_step(self.WORKFLOW, "Test (Windows ARM64)").condition)

    def test_the_none_ran_check_and_the_unexpected_skip_allowlist_remain(self):
        run = find_step(self.WORKFLOW, self.STEP).run
        self.assertIn("if ($ran -eq 0)", run)
        self.assertIn("$expectedSkips = @(", run)
        self.assertIn("skipped that aren't expected to", run)

    def test_arm64_skips_are_honored_on_arm64_only_and_carry_a_reason(self):
        run = find_step(self.WORKFLOW, self.STEP).run
        self.assertIn("$arm64Skips = @{", run)
        self.assertIn("if ($env:RUNNER_ARCH -eq 'ARM64')", run)
        self.assertIn("$arm64Skips[$name]", run)
        # They join the allowlist before the check that uses it, never after.
        self.assertLess(run.index("$expectedSkips += $name"), run.index("$unexpected = "))

    def test_the_memory_compression_test_may_skip_on_x64_only(self):
        # #3839: windows-2025 (Server) leaves memory compression off; windows-11-arm (client) runs the test, so a
        # skip there must fail rather than pass as expected.
        run = find_step(self.WORKFLOW, self.STEP).run
        name = "'CiRealSystemMemoryCompressionTest.TheProbeReadsTheCompressionStoresWorkingSet'"
        every_leg = run[run.index("$expectedSkips = @(") : run.index(")", run.index("$expectedSkips = @("))]
        self.assertNotIn(name, every_leg)
        x64 = run[run.index("$x64Skips = @{") : run.index("}", run.index("$x64Skips = @{"))]
        self.assertIn(name + " = '", x64)
        self.assertIn("if ($env:RUNNER_ARCH -ne 'ARM64')", run)
        self.assertIn("$x64Skips[$name]", run)
        self.assertLess(run.index("$x64Skips = @{"), run.index("$unexpected = "))


class ReleaseConfigurationBuiltBeforeTheTagTests(unittest.TestCase):
    """#4349: the Windows benchmarks and the ARM64 release builds compile in nightly CI, not first on the tag."""

    REUSABLE = WORKFLOWS / "reusable-build-test.yml"
    ARM64 = WORKFLOWS / "arm64.yml"
    NIGHTLY_ONLY = "github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'"

    def job_block(self, workflow: Path, job: str) -> str:
        text = workflow.read_text(encoding="utf-8")
        return text.split(f"\n  {job}:\n", 1)[1].split("\n\n", 1)[0]

    def test_the_windows_benchmark_step_builds_the_target_without_running_it(self):
        step = find_step(self.REUSABLE, "Build benchmarks (Windows)")
        for part in ("runner.os == 'Windows'", "inputs.build_benchmarks", "steps.build-windows.outcome == 'success'"):
            self.assertIn(part, step.condition)
        self.assertIn("-DTASKSMACK_BUILD_BENCHMARKS=ON", step.run)
        self.assertIn("--target TaskSmackBenchmarks", step.run)
        self.assertNotIn("TaskSmackBenchmarks.exe", step.run)

    def test_the_windows_release_nightly_turns_the_benchmark_build_on(self):
        block = self.job_block(WORKFLOWS / "ci.yml", "build-windows-release")
        self.assertIn(self.NIGHTLY_ONLY, block)
        self.assertIn("build_type: release", block)
        self.assertIn("build_benchmarks: true", block)


    def test_arm64_release_legs_run_on_the_schedule_and_manual_runs_only(self):
        for job, os_name in (("build-linux-arm64-release", "ubuntu-26.04-arm"), ("build-windows-arm64-release", "windows-11-arm")):
            with self.subTest(job=job):
                block = self.job_block(self.ARM64, job)
                self.assertIn("needs.detect.outputs.run == 'true'", block)
                self.assertIn(self.NIGHTLY_ONLY, block)
                self.assertNotIn("pull_request", block)
                self.assertIn(f"os: {os_name}", block)
                self.assertIn("build_type: release", block)
                self.assertIn("build_benchmarks: true", block)

    def test_the_windows_arm64_release_leg_builds_without_running_tests(self):
        self.assertIn("run_tests: false", self.job_block(self.ARM64, "build-windows-arm64-release"))
        self.assertNotIn("run_tests", self.job_block(self.ARM64, "build-linux-arm64-release"))
        for name in ("Test (Windows)", "Test (Windows ARM64)", "Real-system tests (Windows)", "Software OpenGL for the display tests (Windows)"):
            with self.subTest(step=name):
                self.assertIn("inputs.run_tests", find_step(self.REUSABLE, name).condition)

    def test_the_debug_arm64_legs_and_their_names_are_unchanged(self):
        text = self.ARM64.read_text(encoding="utf-8")
        for job in ("build-linux-arm64", "build-windows-arm64"):
            block = self.job_block(self.ARM64, job)
            self.assertIn("build_type: debug", block)
            self.assertNotIn("build_benchmarks", block)
        self.assertIn("name: ARM64\n", text)


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

    def test_iwyu_build_deps_get_their_own_entry_and_leave_the_others_keys_alone(self):
        _, plain = self.outputs()
        _, off = self.outputs(INSTALL_IWYU_BUILD_DEPS="false")
        result, deps = self.outputs(INSTALL_IWYU_BUILD_DEPS="true")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(plain["key"], off["key"])
        self.assertNotEqual(plain["restore-keys"], deps["restore-keys"])
        set_id = hashlib.sha256(b"format=false tidy=false iwyu-build-deps=true").hexdigest()[:12]
        self.assertIn(f"-{set_id}-", deps["key"])

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

    def test_iwyu_build_deps_come_from_apt_llvm_org_or_the_cache(self):
        # A set cached without them goes to apt.llvm.org, which installs them with the toolchain (#4481).
        self.fill_cache(NEW_BUILD)
        result, out, calls = self.run_install(CACHE_RESTORED="true", INSTALL_IWYU_BUILD_DEPS="true")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("The LLVM package cache has no llvm-23-dev", result.stdout)
        self.assertEqual(out["cache-hit"], "false")
        installs = [call for call in calls if call.startswith("apt-get install")]
        self.assertIn("llvm-23-dev", installs[-1])
        self.assertIn("libclang-23-dev", installs[-1])
        # A set cached with them installs offline, from the cache.
        self.fill_cache(NEW_BUILD, LLVM_NAMED + ["libllvm23", "llvm-23-dev", "libclang-23-dev"])
        result, out, calls = self.run_install(CACHE_RESTORED="true", INSTALL_IWYU_BUILD_DEPS="true")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out["cache-hit"], "true")
        self.assertFalse([call for call in calls if "apt.llvm.org" in call or "llvm.sh" in call], calls)
        installs = [call for call in calls if call.startswith("apt-get install")]
        self.assertIn("libclang-23-dev_", installs[0])

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


class IwyuBuildToolchainTests(unittest.TestCase):
    """#4481: include-analysis builds include-what-you-use against LLVM from setup-llvm's pinned source. Ubuntu
    26.04's own archive has no LLVM 23, so a bare apt-get of libclang-23-dev there failed every IWYU cache miss."""

    CI = WORKFLOWS / "ci.yml"

    def job_text(self) -> str:
        """The include-analysis job's text: from its header to the next job's."""
        text = self.CI.read_text(encoding="utf-8")
        start = text.index("\n  include-analysis:\n") + 1
        following = re.search(r"^  [a-z][\w-]*:$", text[start + 1 :], re.MULTILINE)
        return text[start:] if following is None else text[start : start + 1 + following.start()]

    def test_the_job_installs_the_iwyu_build_deps_through_setup_llvm(self):
        text = self.job_text()
        setup = text[text.index("uses: ./.github/actions/setup-llvm") :]
        setup = setup[: setup.index("\n\n")]
        self.assertIn("install-iwyu-build-deps: 'true'", setup)

    def test_no_step_installs_llvm_packages_from_the_distro_archive(self):
        for name in step_names(self.CI, "include-analysis"):
            run = find_step(self.CI, name, "include-analysis").run
            with self.subTest(step=name):
                self.assertNotRegex(run, r"apt(-get)?\s+install", "LLVM comes from setup-llvm's pinned source")
                self.assertNotIn("libclang-", run)

    def test_the_iwyu_cache_key_is_unchanged(self):
        text = self.job_text()
        self.assertIn(
            "key: iwyu-llvm${{ env.LLVM_VERSION }}-${{ env.IWYU_COMMIT_SHA }}-${{ runner.os }}-ubuntu-26.04-v1", text
        )
        build = find_step(self.CI, "Build include-what-you-use from source", "include-analysis")
        self.assertEqual(build.condition, "steps.cache-iwyu.outputs.cache-hit != 'true'")


class WingetRehearsalTests(unittest.TestCase):
    """release.yml rehearse-winget (#3822): a dry run exercises the WinGet manifests without the means to submit."""

    @staticmethod
    def job(name: str) -> list[str]:
        """The job's lines, comments left out."""
        lines = (WORKFLOWS / "release.yml").read_text(encoding="utf-8").splitlines()
        start = next(i for i, line in enumerate(lines) if line == f"  {name}:")
        end = next((i for i in range(start + 1, len(lines)) if re.match(r"^  [A-Za-z0-9_-]+:\s*$", lines[i])), len(lines))
        return [line for line in lines[start:end] if not line.lstrip().startswith("#")]

    def test_the_rehearsal_has_no_secret_environment_or_submission(self):
        text = "\n".join(self.job("rehearse-winget"))
        for forbidden in ("secrets.", "WINGET_TOKEN", "WINGET_CREATE_GITHUB_TOKEN", "environment:", "submit", "--submit", "id-token"):
            self.assertNotIn(forbidden, text, f"rehearse-winget mentions {forbidden}")
        self.assertRegex(text, r"(?m)^    permissions:\n      contents: read")
        # Only `wingetcreate update` is run, never another subcommand.
        self.assertEqual(re.findall(r"WINGETCREATE update|WINGETCREATE submit|WINGETCREATE \w+", text), ["WINGETCREATE update"])

    def test_the_package_check_ends_with_exit_zero(self):
        # `gh api` exits 1 on the 404 that means "not in winget-pkgs yet"; pwsh's wrapper ends a step with the last
        # native exit code, so without an explicit exit the dry run failed on the case it is meant to pass.
        step = find_step(WORKFLOWS / "release.yml", "Check whether TaskSmack.TaskSmack is in winget-pkgs", job="rehearse-winget")
        self.assertEqual(step.run.rstrip().splitlines()[-1].strip(), "exit 0")

    @unittest.skipUnless(PWSH, "needs pwsh")
    def test_every_step_script_parses_as_powershell(self):
        # A backtick before a `u` (as in "`update`" inside a double-quoted string) is a Unicode escape and a parse
        # error: the first dry run failed on one.
        for name in step_names(WORKFLOWS / "release.yml", "rehearse-winget"):
            run = find_step(WORKFLOWS / "release.yml", name, job="rehearse-winget").run
            if not run:
                continue
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "step.ps1"
                path.write_text(run + "\n", encoding="utf-8")
                parse = (
                    "$e = $null; [System.Management.Automation.Language.Parser]::ParseFile("
                    f"'{path}', [ref]$null, [ref]$e) | Out-Null; $e | ForEach-Object {{ $_.Message }}; exit $e.Count"
                )
                result = subprocess.run(
                    [PWSH, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", parse],
                    capture_output=True, text=True, encoding="utf-8", check=False, timeout=60,
                )
                self.assertEqual(result.returncode, 0, f"{name}: {result.stdout}{result.stderr}")

    def test_the_rehearsal_runs_on_a_dry_run_only(self):
        condition = "".join(self.job("rehearse-winget")).split("if: >-")[1].split("runs-on:")[0]
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("inputs.dry_run", condition)

    def test_only_publish_winget_reads_the_winget_token_and_only_on_a_tag_push(self):
        text = (WORKFLOWS / "release.yml").read_text(encoding="utf-8")
        uncommented = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
        holders = [line for line in uncommented if "secrets.WINGET_TOKEN" in line]
        self.assertEqual(len(holders), 2, holders)  # the token check's env and the submit step's env
        publish = [line for line in self.job("publish-winget") if "secrets.WINGET_TOKEN" in line]
        self.assertEqual(holders, publish)
        self.assertEqual(len([line for line in self.job("publish-winget") if "WINGETCREATE submit" in line]), 1)
        condition = "".join(self.job("publish-winget")).split("if: >-")[1].split("runs-on:")[0]
        self.assertIn("!(github.event_name == 'workflow_dispatch' && inputs.dry_run)", condition)
        self.assertNotIn("github.event_name == 'push'", condition, "a dry_run=false retry must reach WinGet (#4448)")
        self.assertIn("needs.create-release.outputs.latest == 'true'", condition, "still guarded by Latest only")

    def test_a_dry_run_never_shares_the_publish_concurrency_group(self):
        """#4447: the shared group keeps one pending job, so a dry run's create-release could cancel a tag run's."""
        text = "\n".join(self.job("create-release"))
        group = re.search(r"(?m)^      group: (.+)$", text.split("    concurrency:\n", 1)[1]).group(1)
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.dry_run", group)
        self.assertIn("format('release-publish-dry-{0}-{1}', github.ref, inputs.dry_run)", group)
        self.assertTrue(group.endswith("|| 'release-publish' }}"), group)
        self.assertIn("cancel-in-progress: false", text)

    def test_create_release_waits_for_the_rehearsal(self):
        lines = self.job("create-release")
        self.assertIn("rehearse-winget", next(line for line in lines if line.startswith("    needs:")))
        self.assertTrue(any("needs.rehearse-winget.result == 'success'" in line and "'skipped'" in line for line in lines))

    def test_the_wingetcreate_pins_are_the_workflows_own_and_listed_once(self):
        text = (WORKFLOWS / "release.yml").read_text(encoding="utf-8")
        for pin in ("WINGETCREATE_VERSION", "WINGETCREATE_SHA256", "WINGETCREATE_MANIFEST_VERSION"):
            self.assertEqual(len(re.findall(rf"(?m)^  {pin}: ", text)), 1, pin)
            self.assertEqual(len(re.findall(rf"(?m)^\s+{pin}: ", text)), 1, pin)


# ---- Merge queue (#3818) --------------------------------------------------------------------------------------------

# The status checks the "Main Builds" ruleset requires. UPDATE THIS LIST WHENEVER THE RULESET'S REQUIRED CHECKS
# CHANGE: with a merge queue on, every one of them must report on a merge group, or the queue waits on it forever.
REQUIRED_CHECKS = (
    "CI Success",
    "Run pre-commit checks",
    "OSV Scan Required",
    "Dependency Review",
    "Static Analysis (Windows)",
    "Analyze C++",
)

QUEUE_SHA = "0123456789abcdef0123456789abcdef01234567"
QUEUE_REF = f"refs/heads/gh-readonly-queue/main/pr-3818-{QUEUE_SHA}"


def queue_context(ref: str = QUEUE_REF, sha: str = QUEUE_SHA, base: str = "main") -> dict[str, object]:
    """The `github` context of a merge_group run: no pull request, and a gh-readonly-queue ref of its own."""
    return {
        "github.event_name": "merge_group",
        "github.ref": ref,
        "github.ref_name": ref[len("refs/heads/"):],
        "github.sha": sha,
        "github.repository": "example/tasksmack",
        "github.event.merge_group.base_ref": f"refs/heads/{base}",
        "github.event.merge_group.base_sha": "f" * 40,
        "github.event.merge_group.head_sha": sha,
        "github.event.merge_group.head_ref": ref,
    }


def pull_request_context(number: int = 42, sha: str = "a" * 40) -> dict[str, object]:
    return {
        "github.event_name": "pull_request",
        "github.ref": f"refs/pull/{number}/merge",
        "github.ref_name": f"{number}/merge",
        "github.sha": sha,
        "github.repository": "example/tasksmack",
        "github.base_ref": "main",
        "github.head_ref": "feature",
        "github.event.pull_request.number": str(number),
        "github.event.pull_request.head.repo.full_name": "example/tasksmack",
    }


def push_context(ref: str = "refs/heads/main", sha: str = "b" * 40) -> dict[str, object]:
    return {
        "github.event_name": "push",
        "github.ref": ref,
        "github.ref_name": ref[len("refs/heads/"):],
        "github.sha": sha,
        "github.repository": "example/tasksmack",
    }


_EXPR_TOKEN = re.compile(
    r"\s*(?:(?P<str>'(?:[^']|'')*')|(?P<num>\d+(?:\.\d+)?)|(?P<op>==|!=|&&|\|\||<=|>=|[!()<>,])"
    r"|(?P<id>[A-Za-z_][A-Za-z0-9_-]*(?:\.(?:\*|[A-Za-z_][A-Za-z0-9_-]*))*))"
)
_STATUS_FUNCTIONS = ("always", "success", "failure", "cancelled")


class Expression:
    """A GitHub Actions expression evaluator for the subset these workflows' `if:` and `group:` use: literals,
    context paths, `!`, `==`/`!=`, `&&`/`||` (which return an operand, as GitHub's do), parentheses and
    always()/success()/failure()/cancelled()/startsWith()/contains(). An unknown context path is null."""

    def __init__(self, text: str, context: dict[str, object]):
        self.text = text.strip()
        self.tokens: list[tuple[str, str]] = []
        position = 0
        while position < len(self.text):
            match = _EXPR_TOKEN.match(self.text, position)
            if match is None or match.end() == position:
                raise ValueError(f"can't tokenize {self.text[position:]!r} in {self.text!r}")
            kind = match.lastgroup or ""
            self.tokens.append((kind, match.group(kind)))
            position = match.end()
            while position < len(self.text) and self.text[position].isspace():
                position += 1
        self.index = 0
        self.context = context

    @staticmethod
    def truthy(value: object) -> bool:
        return value not in (None, False, "", 0)

    @staticmethod
    def loose(value: object) -> str:
        """The value as GitHub compares it: strings ignore case, booleans are true/false, null is empty."""
        if value is None:
            return ""
        if value is True:
            return "true"
        if value is False:
            return "false"
        return str(value).lower()

    def peek(self) -> tuple[str, str] | None:
        return self.tokens[self.index] if self.index < len(self.tokens) else None

    def take(self, value: str | None = None) -> tuple[str, str]:
        token = self.peek()
        if token is None or (value is not None and token[1] != value):
            raise ValueError(f"expected {value!r} at token {self.index} of {self.text!r}")
        self.index += 1
        return token

    def evaluate(self) -> object:
        value = self.or_()
        if self.peek() is not None:
            raise ValueError(f"trailing tokens in {self.text!r}")
        return value

    def or_(self) -> object:
        value = self.and_()
        while self.peek() == ("op", "||"):
            self.take()
            right = self.and_()
            value = value if self.truthy(value) else right
        return value

    def and_(self) -> object:
        value = self.compare()
        while self.peek() == ("op", "&&"):
            self.take()
            right = self.compare()
            value = right if self.truthy(value) else value
        return value

    def compare(self) -> object:
        value = self.unary()
        while self.peek() in (("op", "=="), ("op", "!=")):
            operator = self.take()[1]
            right = self.unary()
            equal = self.loose(value) == self.loose(right)
            value = equal if operator == "==" else not equal
        return value

    def unary(self) -> object:
        if self.peek() == ("op", "!"):
            self.take()
            return not self.truthy(self.unary())
        return self.primary()

    def primary(self) -> object:
        kind, value = self.take()
        if (kind, value) == ("op", "("):
            inner = self.or_()
            self.take(")")
            return inner
        if kind == "str":
            return value[1:-1].replace("''", "'")
        if kind == "num":
            return float(value)
        if kind != "id":
            raise ValueError(f"unexpected {value!r} in {self.text!r}")
        if value in ("true", "false"):
            return value == "true"
        if value == "null":
            return None
        if self.peek() == ("op", "("):
            self.take()
            args: list[object] = []
            if self.peek() != ("op", ")"):
                args.append(self.or_())
                while self.peek() == ("op", ","):
                    self.take()
                    args.append(self.or_())
            self.take(")")
            return self.call(value, args)
        return self.context.get(value)

    def call(self, name: str, args: list[object]) -> object:
        if name == "always":
            return True
        if name == "success":
            return bool(self.context.get("__success__", True))
        if name in ("failure", "cancelled"):
            return False
        if name == "startsWith":
            return self.loose(args[0]).startswith(self.loose(args[1]))
        if name == "contains":
            haystack = args[0]
            if isinstance(haystack, (list, tuple)):
                return any(self.loose(item) == self.loose(args[1]) for item in haystack)
            return self.loose(args[1]) in self.loose(haystack)
        raise ValueError(f"unsupported function {name}() in {self.text!r}")


def _strip_braces(expression: str) -> str:
    expression = expression.strip()
    if expression.startswith("${{") and expression.endswith("}}"):
        return expression[3:-2].strip()
    return expression


def evaluate(expression: str, context: dict[str, object]) -> object:
    return Expression(_strip_braces(expression), context).evaluate()


def condition_holds(condition: str, context: dict[str, object]) -> bool:
    """Whether a job or step `if:` passes; with no status function in it, GitHub's implicit success() applies."""
    if not condition:
        return bool(context.get("__success__", True))
    condition = _strip_braces(condition)
    if not any(re.search(rf"\b{name}\(", condition) for name in _STATUS_FUNCTIONS):
        condition = f"success() && ({condition})"
    return Expression.truthy(evaluate(condition, context))


def render(text: str, context: dict[str, object]) -> str:
    """Replaces each `${{ expression }}` in `text` by its value in `context` (null renders empty)."""

    def replace(match: re.Match[str]) -> str:
        value = Expression(match.group(1), context).evaluate()
        if isinstance(value, bool):
            return "true" if value else "false"
        return "" if value is None else str(value)

    return re.sub(r"\$\{\{\s*(.*?)\s*\}\}", replace, text)


@dataclass
class WorkflowJob:
    key: str
    name: str = ""
    condition: str = ""
    needs: list[str] = field(default_factory=list)
    with_values: dict[str, str] = field(default_factory=dict)


def _unquote(value: str) -> str:
    value = value.split(" #", 1)[0].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def _inline_list(value: str) -> list[str]:
    value = value.split(" #", 1)[0].strip()
    if value.startswith("[") and value.endswith("]"):
        return [_unquote(item) for item in value[1:-1].split(",") if item.strip()]
    return [_unquote(value)] if value else []


def _code_lines(path: Path) -> list[str]:
    return [line for line in path.read_text(encoding="utf-8").splitlines() if not line.lstrip().startswith("#")]


def _indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _top_level_block(lines: list[str], key: str) -> list[str]:
    """The lines under the top-level `key:` (`on:`, `jobs:`, `concurrency:`), up to the next top-level key."""
    start = next((i for i, line in enumerate(lines) if re.match(rf"^['\"]?{key}['\"]?:\s*$", line)), None)
    if start is None:
        return []
    end = next((i for i in range(start + 1, len(lines)) if lines[i].strip() and _indent_of(lines[i]) == 0), len(lines))
    return lines[start + 1:end]


def _children(block: list[str]) -> dict[str, tuple[str, list[str]]]:
    """Each key at the block's first indent: its inline value and the lines nested under it."""
    present = [line for line in block if line.strip()]
    if not present:
        return {}
    indent = _indent_of(present[0])
    out: dict[str, tuple[str, list[str]]] = {}
    current: str | None = None
    for line in block:
        if line.strip() and _indent_of(line) == indent and not line.lstrip().startswith("- "):
            key, _, value = line.strip().partition(":")
            current = _unquote(key)
            out[current] = (value.strip(), [])
        elif current is not None:
            out[current][1].append(line)
    return out


def _list_value(inline: str, nested: list[str]) -> list[str]:
    if inline:
        return _inline_list(inline)
    return [_unquote(line.strip()[2:]) for line in nested if line.strip().startswith("- ")]


def _scalar(inline: str, nested: list[str]) -> str:
    if inline in (">", ">-", "|", "|-"):
        return " ".join(line.strip() for line in nested if line.strip())
    return _unquote(inline)


def workflow_triggers(path: Path) -> dict[str, dict[str, tuple[str, list[str]]]]:
    """Each `on:` event and its keys (`branches`, `types`, ...)."""
    return {event: _children(nested) for event, (_, nested) in _children(_top_level_block(_code_lines(path), "on")).items()}


def workflow_jobs(path: Path) -> dict[str, WorkflowJob]:
    jobs: dict[str, WorkflowJob] = {}
    for key, (_, body) in _children(_top_level_block(_code_lines(path), "jobs")).items():
        job = WorkflowJob(key=key)
        fields = _children(body)
        if "name" in fields:
            job.name = _scalar(*fields["name"])
        if "if" in fields:
            job.condition = _scalar(*fields["if"])
        if "needs" in fields:
            job.needs = _list_value(*fields["needs"])
        if "with" in fields:
            job.with_values = {name: _scalar(*value) for name, value in _children(fields["with"][1]).items()}
        jobs[key] = job
    return jobs


def concurrency_group(path: Path) -> str:
    fields = _children(_top_level_block(_code_lines(path), "concurrency"))
    return _scalar(*fields["group"]) if "group" in fields else ""


def _filter_matches(pattern: str, value: str) -> bool:
    """A GitHub branch filter glob: `**` crosses `/`, `*` doesn't."""
    regex = ""
    index = 0
    while index < len(pattern):
        if pattern.startswith("**", index):
            regex += ".*"
            index += 2
        elif pattern[index] == "*":
            regex += "[^/]*"
            index += 1
        else:
            regex += re.escape(pattern[index])
            index += 1
    return re.fullmatch(regex, value) is not None


def branch_triggers(keys: dict[str, tuple[str, list[str]]], branch: str) -> bool:
    """Whether an event with these filter keys (push or workflow_run) fires for `branch`."""
    if "branches" in keys:
        return any(_filter_matches(pattern, branch) for pattern in _list_value(*keys["branches"]))
    if "branches-ignore" in keys:
        return not any(_filter_matches(pattern, branch) for pattern in _list_value(*keys["branches-ignore"]))
    # A push filtered by tags alone never fires for a branch.
    return "tags" not in keys and "tags-ignore" not in keys


def run_workflow_step(workflow: Path, job: str, step_name: str, context: dict[str, object], directory: Path,
                      prelude: str = "") -> tuple[subprocess.CompletedProcess, dict[str, str]]:
    """Runs one step's bash with its `${{ }}` rendered from `context`; returns the result and its GITHUB_OUTPUT."""
    step = find_step(workflow, step_name, job=job)
    output = directory / "github-output.txt"
    output.write_text("", encoding="utf-8")
    env = {key: render(value, context) for key, value in step.env.items()}
    env.update({
        "GITHUB_OUTPUT": posix(output),
        "GITHUB_STEP_SUMMARY": posix(directory / "summary.md"),
        "RUNNER_TEMP": posix(directory),
    })
    result = run_bash(prelude + render(step.run, context), directory, env)
    outputs = {}
    for line in output.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            outputs[key] = value
    return result, outputs


class MergeQueueTests(unittest.TestCase):
    """#3818: every required check runs, and can pass, on a merge queue's merge group."""

    GATE_STEP = "Check all required jobs passed"

    @staticmethod
    def producers(check: str) -> list[tuple[Path, WorkflowJob]]:
        """The jobs named `check` in workflows a pull request triggers (static-analysis.yml's dispatch-only
        "Static Analysis (Windows)" never reports on a pull request, so it is not the required one)."""
        found = []
        for path in sorted(WORKFLOWS.glob("*.yml")):
            if "pull_request" not in workflow_triggers(path):
                continue
            for job in workflow_jobs(path).values():
                if job.name == check:
                    found.append((path, job))
        return found

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="3818-merge-queue-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def simulate(self, path: Path, context: dict[str, object]) -> tuple[dict[str, str], dict[str, dict[str, str]]]:
        """Every job's result ('success' or 'skipped') for a run in which every job that starts succeeds; the
        detect jobs' outputs come from running their own steps."""
        jobs = workflow_jobs(path)
        results: dict[str, str] = {}
        outputs: dict[str, dict[str, str]] = {}
        pending = list(jobs)
        while pending:
            ready = [key for key in pending if all(need in results for need in jobs[key].needs)]
            self.assertTrue(ready, f"{path.name}: a needs cycle or an unknown job among {pending}")
            for key in ready:
                job = jobs[key]
                scope = self.with_needs(context, {need: results[need] for need in job.needs}, outputs)
                scope["__success__"] = all(results[need] == "success" for need in job.needs)
                results[key] = "success" if condition_holds(job.condition, scope) else "skipped"
                outputs[key] = self.job_outputs(path, job, scope) if results[key] == "success" else {}
                pending.remove(key)
        return results, outputs

    def job_outputs(self, path: Path, job: WorkflowJob, scope: dict[str, object]) -> dict[str, str]:
        detect_steps = {
            ("ci.yml", "detect-docs-only"): "Classify the changes",
            ("codeql.yml", "detect-docs-only"): "Normalize output",
            ("osv-scanner.yml", "detect-relevant-changes"): "Normalize output",
        }
        step = detect_steps.get((path.name, job.key))
        if step is not None:
            result, out = run_workflow_step(path, job.key, step, scope, self.tmp)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return out
        if "clang_tidy" in job.with_values:
            # reusable-build-test.yml's tidy outputs for a run whose clang-tidy passed on every file.
            ran = Expression.truthy(evaluate(job.with_values["clang_tidy"], scope))
            return {"tidy-mode": "all", "tidy-outcome": "success"} if ran else {"tidy-mode": "", "tidy-outcome": ""}
        return {}

    @staticmethod
    def with_needs(context: dict[str, object], results: dict[str, str],
                   outputs: dict[str, dict[str, str]]) -> dict[str, object]:
        scope = dict(context)
        for key, result in results.items():
            scope[f"needs.{key}.result"] = result
            for name, value in outputs.get(key, {}).items():
                scope[f"needs.{key}.outputs.{name}"] = value
        return scope

    def run_ci_gate(self, context: dict[str, object], results: dict[str, str],
                    outputs: dict[str, dict[str, str]]) -> subprocess.CompletedProcess:
        scope = self.with_needs(context, results, outputs)
        result, _ = run_workflow_step(WORKFLOWS / "ci.yml", "ci-success", self.GATE_STEP, scope, self.tmp)
        return result

    def test_every_required_check_has_a_producer_with_the_merge_group_trigger(self):
        for check in REQUIRED_CHECKS:
            with self.subTest(check=check):
                producers = self.producers(check)
                self.assertTrue(producers, f"no pull_request workflow has a job named {check!r}; update REQUIRED_CHECKS")
                for path, _job in producers:
                    triggers = workflow_triggers(path)
                    self.assertIn("merge_group", triggers, f"{path.name} reports {check!r} but has no merge_group trigger")
                    self.assertEqual(_list_value(*triggers["merge_group"]["types"]), ["checks_requested"], path.name)

    def test_no_required_check_is_skipped_on_a_merge_group(self):
        for check in REQUIRED_CHECKS:
            for path, job in self.producers(check):
                with self.subTest(check=check, workflow=path.name):
                    results, _ = self.simulate(path, queue_context())
                    self.assertEqual(results[job.key], "success", f"{check!r} ({path.name} {job.key}) is skipped")

    def test_ci_runs_the_pull_request_gate_jobs_on_a_merge_group_and_ci_success_passes(self):
        results, outputs = self.simulate(WORKFLOWS / "ci.yml", queue_context())
        # The classifier gives a merge group every job: nothing the PR gate builds is skipped on the combined tree.
        self.assertEqual(outputs["detect-docs-only"], {
            "docs_only": "false", "linux_only": "false", "windows_only": "false", "automation_only": "false",
            "docs_site": "true", "dev_tools_only": "false",
        })
        for key in ("build-linux-debug", "build-linux-release", "build-windows-debug", "static-analysis-windows",
                    "asan-ubsan", "docs-hygiene"):
            self.assertEqual(results[key], "success", f"{key} doesn't run on a merge group")
        self.assertEqual(outputs["build-linux-debug"]["tidy-mode"], "all")
        self.assertEqual(outputs["build-windows-debug"]["tidy-mode"], "all")
        result = self.run_ci_gate(queue_context(), results, outputs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("All required CI jobs passed", result.stdout)

    def test_ci_success_fails_a_merge_group_whose_sanitizer_build_or_tidy_job_did_not_run(self):
        results, outputs = self.simulate(WORKFLOWS / "ci.yml", queue_context())
        for key in ("asan-ubsan", "build-linux-debug", "build-windows-debug", "static-analysis-windows"):
            with self.subTest(job=key):
                result = self.run_ci_gate(queue_context(), {**results, key: "skipped"}, outputs)
                self.assertNotEqual(result.returncode, 0, f"CI Success passed a merge group with {key} skipped")

    def test_pull_request_and_push_runs_are_unchanged(self):
        # A pull request still runs ASan+UBSan and the Windows tidy report; a push to main still runs neither.
        pr_results, pr_outputs = self.simulate(WORKFLOWS / "ci.yml", pull_request_context())
        self.assertEqual(pr_results["asan-ubsan"], "success")
        self.assertEqual(pr_results["static-analysis-windows"], "success")
        self.assertEqual(pr_results["automation-checks"], "skipped")
        push_results, push_outputs = self.simulate(WORKFLOWS / "ci.yml", push_context())
        self.assertEqual(push_results["asan-ubsan"], "skipped")
        self.assertEqual(push_results["static-analysis-windows"], "skipped")
        self.assertEqual(push_outputs["build-linux-debug"]["tidy-mode"], "")
        result = self.run_ci_gate(push_context(), push_results, push_outputs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # A pull request's gate still requires ASan+UBSan.
        result = self.run_ci_gate(pull_request_context(), {**pr_results, "asan-ubsan": "skipped"}, pr_outputs)
        self.assertNotEqual(result.returncode, 0)

    def test_a_dev_tools_only_pull_request_skips_the_cpp_jobs_and_passes_on_the_dev_tool_checks(self):
        # #4248: with dev_tools_only, every C++ job skips, both dev-tools-checks jobs run, and CI Success passes on
        # them; with them failed, it fails.
        jobs = workflow_jobs(WORKFLOWS / "ci.yml")
        detect = {"docs_only": "false", "linux_only": "false", "windows_only": "false", "automation_only": "false",
                  "docs_site": "false", "dev_tools_only": "true"}
        results: dict[str, str] = {"detect-docs-only": "success"}
        outputs: dict[str, dict[str, str]] = {"detect-docs-only": detect}
        pending = [key for key in jobs if key != "detect-docs-only"]
        while pending:
            ready = [key for key in pending if all(need in results for need in jobs[key].needs)]
            self.assertTrue(ready, pending)
            for key in ready:
                scope = self.with_needs(pull_request_context(), {need: results[need] for need in jobs[key].needs}, outputs)
                scope["__success__"] = all(results[need] == "success" for need in jobs[key].needs)
                results[key] = "success" if condition_holds(jobs[key].condition, scope) else "skipped"
                outputs[key] = {}
                pending.remove(key)
        for key in ("build-linux-debug", "build-linux-release", "build-windows-debug", "static-analysis-windows",
                    "asan-ubsan", "automation-checks"):
            self.assertEqual(results[key], "skipped", key)
        for key in ("dev-tools-checks", "dev-tools-checks-windows", "docs-hygiene"):
            self.assertEqual(results[key], "success", key)
        result = self.run_ci_gate(pull_request_context(), results, outputs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_ci_gate(pull_request_context(), {**results, "dev-tools-checks-windows": "failure"}, outputs)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_osv_scan_required_scans_and_passes_on_a_merge_group(self):
        path = WORKFLOWS / "osv-scanner.yml"
        results, outputs = self.simulate(path, queue_context())
        self.assertEqual(outputs["detect-relevant-changes"], {"relevant": "true"})
        self.assertEqual(results["generate-sbom"], "success")
        self.assertEqual(results["vulnerability-scan"], "success")
        scope = self.with_needs(queue_context(), results, outputs)
        result, _ = run_workflow_step(path, "osv-scan-required", "Check scan result", scope, self.tmp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Vulnerability scan passed.", result.stdout)

    def test_codeql_analyzes_a_merge_group_in_full(self):
        results, outputs = self.simulate(WORKFLOWS / "codeql.yml", queue_context())
        self.assertEqual(outputs["detect-docs-only"], {"docs_only": "false"})
        self.assertEqual(results["analyze"], "success")

    def test_dependency_review_uses_an_action_that_reads_the_merge_group_refs(self):
        # dependency-review-action v5 takes the base and head from merge_group.base_sha/head_sha itself; without
        # that, a merge_group run fails with "Both a base ref and head ref must be provided".
        step = find_step(WORKFLOWS / "dependency-review.yml", "Review dependencies for CVEs", job="dependency-review")
        self.assertRegex(step.uses, r"^actions/dependency-review-action@[0-9a-f]{40} # v([5-9]|\d{2,})\.")
        self.assertEqual(step.condition, "")

    def test_concurrency_groups_never_join_two_queue_entries(self):
        other_sha = "fedcba9876543210fedcba9876543210fedcba98"
        other = queue_context(ref=f"refs/heads/gh-readonly-queue/main/pr-3819-{other_sha}", sha=other_sha)
        paths = sorted({path for check in REQUIRED_CHECKS for path, _ in self.producers(check)})
        for path in paths:
            group = concurrency_group(path)
            if not group:
                continue
            with self.subTest(workflow=path.name):
                self.assertNotEqual(render(group, queue_context()), render(group, other))
                # A pull request's pushes still share one group, so a new push cancels the last one's run.
                self.assertEqual(render(group, pull_request_context(sha="1" * 40)),
                                 render(group, pull_request_context(sha="2" * 40)))

    def test_no_push_or_workflow_run_trigger_fires_for_a_queue_branch(self):
        for branch in (f"gh-readonly-queue/main/pr-3818-{QUEUE_SHA}", f"gh-readonly-queue/release/0.10/pr-3818-{QUEUE_SHA}"):
            for path in sorted(WORKFLOWS.glob("*.yml")):
                triggers = workflow_triggers(path)
                for event in ("push", "workflow_run"):
                    if event in triggers:
                        with self.subTest(workflow=path.name, event=event, branch=branch):
                            self.assertFalse(branch_triggers(triggers[event], branch),
                                             f"{path.name}'s {event} trigger fires for the merge queue's {branch}")
        # The filters still match what they should.
        self.assertTrue(branch_triggers(workflow_triggers(WORKFLOWS / "ci.yml")["push"], "release/0.10"))
        self.assertTrue(branch_triggers(workflow_triggers(WORKFLOWS / "copilot-setup-steps.yml")["push"], "feature/x"))

    def test_merge_groups_never_save_the_clang_tidy_result_cache(self):
        step = find_step(WORKFLOWS / "reusable-build-test.yml", "Save clang-tidy results")
        cases = ((queue_context(), False), (pull_request_context(), False), (push_context(), True),
                 ({**push_context(), "github.event_name": "schedule"}, True))
        for context, saves in cases:
            with self.subTest(event=context["github.event_name"]):
                scope = {**context, "steps.tidy-cache.outcome": "success"}
                self.assertEqual(condition_holds(step.condition, scope), saves)

    def test_the_version_guard_reads_the_queue_base_branch(self):
        prelude = 'python3() { printf "%s\\n" "$*" >> calls.log; }\ngit() { :; }\n'
        step = "Compare PROJECT_VERSION with the published release tags"
        log = self.tmp / "calls.log"
        cases = [
            (queue_context(ref=f"refs/heads/gh-readonly-queue/main/pr-1-{QUEUE_SHA}"), "main"),
            (queue_context(ref=f"refs/heads/gh-readonly-queue/release/0.10/pr-1-{QUEUE_SHA}", base="release/0.10"),
             "release/0.10"),
            ({**pull_request_context(), "github.base_ref": "release/0.9"}, "release/0.9"),
            (push_context(ref="refs/heads/release/0.9"), "release/0.9"),
        ]
        for context, branch in cases:
            with self.subTest(event=context["github.event_name"], branch=branch):
                if log.exists():
                    log.unlink()
                result, _ = run_workflow_step(WORKFLOWS / "pre-commit.yml", "version-ahead", step, context, self.tmp,
                                              prelude=prelude)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"--branch {branch} ", log.read_text(encoding="utf-8"))

    def test_the_expression_evaluator_matches_github_on_the_shapes_used(self):
        context = {"a": "x", "n": None}
        self.assertEqual(evaluate("n || a", context), "x")
        self.assertEqual(evaluate("a && 'y'", context), "y")
        self.assertFalse(Expression.truthy(evaluate("n && a", context)))
        self.assertTrue(evaluate("a == 'X'", context))  # string comparison ignores case
        self.assertTrue(evaluate("!(a != 'x')", context))
        self.assertTrue(evaluate("startsWith('refs/heads/release/1', 'refs/heads/release/')", context))
        self.assertFalse(condition_holds("a == 'x'", {**context, "__success__": False}))
        self.assertTrue(condition_holds("always() && a == 'x'", {**context, "__success__": False}))


def _is_setup_llvm(test: unittest.TestCase) -> bool:
    return type(test).__name__.startswith("SetupLlvm")


def _is_benchmark_gate(test: unittest.TestCase) -> bool:
    """Heavy Checks' benchmark gate: the check, the re-measurement across launches and the second runner."""
    return type(test).__name__.startswith(("HeavyChecksBenchmark", "HeavyChecksAcrossLaunches", "HeavyChecksSecondRunner"))


# Groups ctest runs as entries of their own (--only-<group>), and WorkflowRunStepsScript runs without
# (--skip-<group>, any of them): each spawns bash dozens of times, and on the windows-11-arm runner's slow spawns
# all of them together outran one entry's 120 s timeout (#3643 setup-llvm, #3709 the benchmark gate).
GROUPS = {"setup-llvm": _is_setup_llvm, "benchmark-gate": _is_benchmark_gate}


def _filtered(suite: unittest.TestSuite, keep) -> unittest.TestSuite:
    out = unittest.TestSuite()
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            out.addTests(_filtered(item, keep))
        elif keep(item):
            out.addTest(item)
    return out


@unittest.skipUnless(shutil.which("git"), "git not available")
class CiRevertGuardTests(unittest.TestCase):
    """#3986: the advisory revert guard runs on every pull request and merge group, docs-only ones too, and its
    step warns, passes and writes the summary for a change that reverts a recent merge; the description is data. It
    runs the base branch's copy of the guard, so a branch made before the guard merged is still checked (#4002). Its
    own workflow since #4095, so a label added or removed and a description edited run it again."""

    WORKFLOW = WORKFLOWS / "revert-guard.yml"
    JOB = "revert-guard"
    STEP = "Check for reverts of recent merges"

    def test_the_job_runs_on_every_pull_request_and_merge_group(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split(f"\n  {self.JOB}:\n", 1)[1].split("\n  # ", 1)[0]
        self.assertIn("(github.event_name == 'pull_request' || github.event_name == 'merge_group')", job)
        self.assertNotIn("needs:", job, "no detect job that could skip it on a docs-only change")
        # #4128: only ci:revert-ok among the labels, and only a description edit, can change the verdict; any other
        # label or edit skips the job, and has a concurrency group of its own so it never cancels the real run.
        ignored = (
            "(((github.event.action == 'labeled' || github.event.action == 'unlabeled') &&\n"
        )
        self.assertIn("!" + ignored, job, "a label other than ci:revert-ok skips the job (#4128)")
        self.assertIn("github.event.label.name != 'ci:revert-ok'", job)
        self.assertIn("(github.event.action == 'edited' && !github.event.changes.body)", job)
        concurrency = text.split("\nconcurrency:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("github.event.label.name != 'ci:revert-ok'", concurrency)
        self.assertIn("(github.event.action == 'edited' && !github.event.changes.body)", concurrency)
        self.assertIn("format('-ignored-{0}', github.run_id)", concurrency, "an ignored event can't cancel the real run")
        on = text.split("\non:\n", 1)[1].split("\n\n", 1)[0]
        self.assertNotIn("paths", on, "no paths filter: a required check must report on every change")
        self.assertIn("merge_group:", on)
        for event in ("labeled", "unlabeled", "edited"):
            self.assertIn(event, on, f"a {event} PR runs it again: the label or the description can clear a finding (#4095)")
        self.assertNotIn("revert-guard:", (WORKFLOWS / "ci.yml").read_text(encoding="utf-8"), "not also in ci.yml")
        step = find_step(self.WORKFLOW, self.STEP, job=self.JOB)
        self.assertIn("--advisory", step.run, "advisory until the maintainer requires it (#3986)")
        self.assertNotIn("${{", step.run, "the labels and description reach it through the environment")

    def test_the_step_warns_on_a_stale_revert_and_never_runs_the_description(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / "repo"
            (repo / "tools").mkdir(parents=True)
            shutil.copy(REPO / "tools" / "check-no-reverts.py", repo / "tools" / "check-no-reverts.py")
            env = {**os.environ, "GIT_AUTHOR_NAME": "T", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                   "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.invalid",
                   "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull}

            def git(*args):
                return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True,
                                      env=env).stdout.strip()

            lines = "".join(f"    int value_{i} = compute_the_recent_thing({i});\n" for i in range(8))
            git("init", "-q", "-b", "main")
            (repo / "a.cpp").write_text("// start\n", encoding="utf-8")
            git("add", "-A")
            git("commit", "-q", "-m", "start")
            (repo / "a.cpp").write_text("// start\n" + lines, encoding="utf-8")
            git("commit", "-q", "-am", "feat: recent lines (#901)")
            base = git("rev-parse", "HEAD")
            git("switch", "-q", "-c", "stale")
            (repo / "a.cpp").write_text("// start\n", encoding="utf-8")
            git("commit", "-q", "-am", "fix: over a stale tree (#902)")
            head = git("rev-parse", "HEAD")
            summary = root / "summary.md"
            summary.write_text("", encoding="utf-8")
            marker = root / "pwned"
            step = find_step(self.WORKFLOW, self.STEP, job=self.JOB)
            script = f'set -e\npython() {{ "{posix(Path(sys.executable))}" "$@"; }}\ncd "{posix(repo)}"\n' + step.run
            result = run_bash(script, root, {
                "BASE_SHA": base, "HEAD_SHA": head, "PR_LABELS": "bug", "RUNNER_TEMP": posix(root),
                "GITHUB_STEP_SUMMARY": posix(summary), "PYTHONUTF8": "1",
                "PR_BODY": f'Fixes #902 $(touch "{posix(marker)}") `touch "{posix(marker)}"`',
            })
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("::warning title=Revert guard::This change removes 8 line(s)", result.stdout)
            self.assertIn("feat: recent lines (#901)", summary.read_text(encoding="utf-8"))
            self.assertFalse(marker.exists(), "the description was run as a command")

            # A branch made before the guard merged has no tools/check-no-reverts.py of its own (#4002): the base's
            # copy runs, so the job still checks it and doesn't exit 2.
            git("rm", "-q", "tools/check-no-reverts.py")
            git("commit", "-q", "-m", "fix: an old branch without the guard (#903)")
            (repo / "tools").mkdir(exist_ok=True)
            env_step = {"BASE_SHA": base, "HEAD_SHA": git("rev-parse", "HEAD"), "PR_LABELS": "", "RUNNER_TEMP": posix(root),
                        "GITHUB_STEP_SUMMARY": posix(summary), "PYTHONUTF8": "1", "PR_BODY": ""}
            result = run_bash(script, root, env_step)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("::warning title=Revert guard::This change removes 8 line(s)", result.stdout)

            # A base branch without the guard (an old release branch): nothing to run, and the job passes.
            git("switch", "-q", "--orphan", "old-release")
            (repo / "README.md").write_text("old\n", encoding="utf-8")
            git("add", "README.md")
            git("commit", "-q", "-m", "an old release")
            result = run_bash(script, root, {**env_step, "BASE_SHA": git("rev-parse", "HEAD")})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("not run: the base branch has no tools/check-no-reverts.py", result.stdout)


@unittest.skipUnless(shutil.which("git"), "git not available")
class HeavyChecksPushScopeTests(unittest.TestCase):
    """#3834: Heavy Checks runs on pushes that touch App, Core, main.cpp or tests too, for the coverage jobs, and
    push-scope keeps benchmark-regression to pushes that touch what the benchmarks measure."""

    WORKFLOW = WORKFLOWS / "heavy-checks.yml"
    JOB = "push-scope"
    STEP = "Decide what this push runs"
    COVERAGE_ONLY = {"src/App/**", "src/Core/**", "src/main.cpp", "tests/**"}

    def push_paths(self) -> list[str]:
        text = self.WORKFLOW.read_text(encoding="utf-8")
        block = text.split("\n  push:\n", 1)[1].split("\n  schedule:", 1)[0]
        return re.findall(r"^      - '([^']+)'$", block, re.MULTILINE)

    def test_the_step_lists_the_push_filters_benchmark_paths(self):
        paths = self.push_paths()
        self.assertTrue(self.COVERAGE_ONLY <= set(paths), "the coverage-only paths are in the push filter")
        step = find_step(self.WORKFLOW, self.STEP, job=self.JOB).run
        listed = re.search(r"case \"\$path\" in\n(.*?)\)\n", step, re.DOTALL).group(1)
        patterns = {p.strip() for p in listed.replace("\\\n", "").split("|")}
        expected = {p.replace("/**", "/*") for p in paths if p not in self.COVERAGE_ONLY}
        self.assertEqual(patterns, expected, "push-scope's case list is the push filter's benchmark half")

    def test_benchmark_regression_waits_for_push_scope(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split("\n  benchmark-regression:\n", 1)[1].split("\n  # ", 1)[0]
        self.assertIn("    needs: push-scope\n", job)
        self.assertIn("needs.push-scope.outputs.benchmarks == 'true'", job)
        for coverage in ("coverage", "win-coverage"):
            block = text.split(f"\n  {coverage}:\n", 1)[1].split("\n    runs-on:", 1)[0]
            self.assertNotIn("push-scope", block, f"{coverage} runs on every push the filter lets in")

    def scope(self, event: str, changed: dict[str, str] | None, before: str | None = None) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / "repo"
            repo.mkdir()
            env = {**os.environ, "GIT_AUTHOR_NAME": "T", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                   "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.invalid",
                   "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull}

            def git(*args):
                return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True,
                                      env=env).stdout.strip()

            git("init", "-q", "-b", "main")
            (repo / "README.md").write_text("start\n", encoding="utf-8")
            git("add", "-A")
            git("commit", "-q", "-m", "start")
            first = git("rev-parse", "HEAD")
            for name, text in (changed or {}).items():
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            git("add", "-A")
            git("commit", "-q", "--allow-empty", "-m", "the push")
            output = root / "output"
            output.write_text("", encoding="utf-8")
            step = find_step(self.WORKFLOW, self.STEP, job=self.JOB)
            result = run_bash(f'set -e\ncd "{posix(repo)}"\n' + step.run, root, {
                "EVENT": event, "BEFORE": first if before is None else before, "SHA": git("rev-parse", "HEAD"),
                "GITHUB_OUTPUT": posix(output)})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines())["benchmarks"]

    def test_a_push_to_app_core_main_or_tests_runs_coverage_only(self):
        for name in ("src/App/Panels/ProcessesPanel.cpp", "src/Core/Application.cpp", "src/main.cpp",
                     "tests/App/test_x.cpp"):
            with self.subTest(name=name):
                self.assertEqual(self.scope("push", {name: "x\n"}), "false")

    def test_a_push_to_a_benchmarked_path_runs_the_benchmarks(self):
        for name in ("src/Domain/History.h", "src/Platform/Linux/X.cpp", "src/App/Panels/ProcessDetailsHistory.h",
                     "benchmarks/bench_x.cpp", "tools/bench.sh", ".github/workflows/heavy-checks.yml"):
            with self.subTest(name=name):
                self.assertEqual(self.scope("push", {"src/App/a.cpp": "x\n", name: "x\n"}), "true")

    def test_other_events_and_an_unusable_before_run_the_benchmarks(self):
        self.assertEqual(self.scope("schedule", {"src/App/a.cpp": "x\n"}), "true")
        self.assertEqual(self.scope("workflow_dispatch", {"src/App/a.cpp": "x\n"}), "true")
        self.assertEqual(self.scope("push", {"src/App/a.cpp": "x\n"}, before="0" * 40), "true")
        self.assertEqual(self.scope("push", {"src/App/a.cpp": "x\n"}, before="1" * 40), "true")


# Stands in for every `gh api` call of update_issue() and of the dispatch it leads to, as a shell function
# (a process per call would add about forty launches to a ctest entry that is short of time on ARM64 Windows,
# #3643): logs each call on one line to $CALLS, lists $RELEASE_BRANCHES, finds the open tracking issues named
# in $OPEN_ISSUES ("<branch>=<number>", space-separated), fails the lookup for $FAIL_BRANCH, opens #99, and
# answers the dispatch with status $DISPATCH_STATUS.
DASHBOARD_DISPATCH_GH_STUB = r"""gh() {
  local call="$*" name entry
  printf '%s\n' "${call//$'\n'/ }" >>"$CALLS"
  case "$call" in
    "api --paginate repos/owner/repo/branches?"*)
      for name in ${RELEASE_BRANCHES:-}; do echo "$name"; done ;;
    "api repos/owner/repo/issues?labels="*)
      if [[ -n "${FAIL_BRANCH:-}" && "$call" == *"CI: $FAIL_BRANCH is red"* ]]; then return 1; fi
      for entry in ${OPEN_ISSUES:-}; do
        if [[ "$call" == *"CI: ${entry%%=*} is red"* ]]; then echo "${entry##*=}"; fi
      done ;;
    "api -X POST repos/owner/repo/actions/workflows/release-dashboard.yml/dispatches "*)
      return "${DISPATCH_STATUS:-0}" ;;
    "api repos/owner/repo/issues -f title="*) echo 99 ;;
    "api repos/owner/repo/issues/"*" --jq .body") echo '- **Sanitizers** (push): `failure`' ;;
    "api repos/owner/repo/labels "*|"api -X PATCH repos/owner/repo/issues/"*|"api repos/owner/repo/issues/"*"/comments "*) ;;
    *) echo "gh stub: unexpected command $call" >&2; return 2 ;;
  esac
}
sleep() { :; }
"""


class MainHealthTellsTheDashboardTests(unittest.TestCase):
    """#4700: Main Health opens and closes its tracking issue with the workflow token, which starts no workflow,
    so the release dashboard only saw main turn red or green at its next run. A run that opened or closed an
    issue now dispatches the dashboard, once, and a dispatch that fails doesn't fail the check."""

    WORKFLOW = WORKFLOWS / "main-health.yml"
    DISPATCH = "api -X POST repos/owner/repo/actions/workflows/release-dashboard.yml/dispatches --silent -f ref=main"
    # name: (the branches judged red, the tracking issues open before the run, the release branches that exist,
    #        the dispatch call's exit status, the branch whose issue lookup fails)
    CASES = {
        "green, no issue": ("", "", "", 0, ""),
        "main turns red": ("main", "", "", 0, ""),
        "main stays red": ("main", "main=7", "", 0, ""),
        "main turns green": ("", "main=7", "", 0, ""),
        "two branches change in one run": ("main", "release/0.9=8", "release/0.9", 0, ""),
        "the dispatch fails": ("main", "", "", 1, ""),
        "a later branch fails after main's issue opened": ("main", "", "release/0.9", 0, "release/0.9"),
    }

    @classmethod
    def setUpClass(cls):
        """Every case in one bash process, each in a subshell: the step's own update_issue(), its exit trap and
        its loop, with judge_branch() answering from the case and `gh` a stub that logs what it's asked."""
        step = find_step(cls.WORKFLOW, MAIN_HEALTH_STEP)
        cut = step.run.index("\n}\n", step.run.index("update_issue() {")) + 3
        functions, selection = step.run[:cut], step.run[cut:]
        tmp = Path(tempfile.mkdtemp(prefix="4700-main-health-dispatch-"))
        cls.addClassCleanup(shutil.rmtree, tmp, ignore_errors=True)
        judge = (
            'judge_branch() {\n'
            '  red=""\n'
            '  case " ${RED_BRANCHES:-} " in *" $1 "*) red="- **CI** (push): failure"$\'\\n\' ;; esac\n'
            '}\n'
        )
        script = []
        for index, (red, open_issues, release_branches, status, fails) in enumerate(cls.CASES.values()):
            script += [
                f'( export RED_BRANCHES="{red}" OPEN_ISSUES="{open_issues}" RELEASE_BRANCHES="{release_branches}"',
                f'  export DISPATCH_STATUS="{status}" FAIL_BRANCH="{fails}" CALLS="$DIR/calls-{index}"',
                '  : > "$CALLS"',
                functions,
                DASHBOARD_DISPATCH_GH_STUB,
                judge,
                selection,
                f') > "$DIR/log-{index}" 2>&1',
                f'echo "$?" > "$DIR/status-{index}"',
            ]
        env = {
            "DIR": posix(tmp),
            "REPO": "owner/repo",
            "SERVER": "https://github.example",
            "LABEL": "ci-red-main",
            "GITHUB_WORKFLOW": "Main Health",
            "GITHUB_RUN_ID": "42",
            "TRIGGER_BRANCH": "main",
            "TRIGGER_NAME": "CI",
            "TRIGGER_URL": "https://github.example/runs/1",
            "TRIGGER_CONCLUSION": "failure",
            "TRIGGER_SHA": "0123456789abcdef",
        }
        cls.result = run_bash("\n".join(script), tmp, env)

        def text(kind: str, index: int) -> str:
            path = tmp / f"{kind}-{index}"
            return path.read_text(encoding="utf-8").strip() if path.is_file() else ""

        cls.seen = {
            name: {"calls": text("calls", index).splitlines(), "log": text("log", index), "status": text("status", index)}
            for index, name in enumerate(cls.CASES)
        }

    def dispatches(self, name: str, status: str = "0") -> list[str]:
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        seen = self.seen[name]
        self.assertEqual(seen["status"], status, seen["log"])
        return [call for call in seen["calls"] if "/dispatches " in call]

    def test_an_issue_opening_starts_one_dashboard_run(self):
        self.assertEqual(self.dispatches("main turns red"), [self.DISPATCH])
        self.assertIn("Started a Release Dashboard run: opened #99 for main.", self.seen["main turns red"]["log"])

    def test_an_issue_closing_starts_one_dashboard_run(self):
        self.assertEqual(self.dispatches("main turns green"), [self.DISPATCH])
        calls = self.seen["main turns green"]["calls"]
        self.assertTrue(any(call.startswith("api -X PATCH repos/owner/repo/issues/7 ") and "state=closed" in call for call in calls), calls)
        self.assertIn("Started a Release Dashboard run: closed #7 for main.", self.seen["main turns green"]["log"])

    def test_no_change_starts_none(self):
        # Green with nothing open, and red with its issue already open (the issue's text is rewritten, and the
        # dashboard's own reads show which workflows are red).
        self.assertEqual(self.dispatches("green, no issue"), [])
        self.assertEqual(self.dispatches("main stays red"), [])
        calls = self.seen["main stays red"]["calls"]
        self.assertTrue(any(call.startswith("api -X PATCH repos/owner/repo/issues/7 ") and "-f body=" in call for call in calls), calls)

    def test_two_branches_changing_in_one_run_start_one_run_not_two(self):
        self.assertEqual(self.dispatches("two branches change in one run"), [self.DISPATCH])
        self.assertIn("Started a Release Dashboard run: opened #99 for main; closed #8 for release/0.9.",
                      self.seen["two branches change in one run"]["log"])

    def test_a_dispatch_that_fails_is_a_warning_and_the_check_still_passes(self):
        self.assertEqual(self.dispatches("the dispatch fails"), [self.DISPATCH])
        log = self.seen["the dispatch fails"]["log"]
        self.assertIn("Opened #99.", log)
        self.assertIn("::warning title=Main Health::The Release Dashboard run couldn't be started (opened #99 for main).", log)

    def test_a_run_that_fails_after_changing_an_issue_still_tells_the_dashboard(self):
        # main's issue opens, then the next branch's lookup fails all four tries and the step exits 1. The
        # next run finds main's issue already open and has nothing to say, so this run's exit says it.
        name = "a later branch fails after main's issue opened"
        self.assertEqual(self.dispatches(name, status="1"), [self.DISPATCH])
        log = self.seen[name]["log"]
        self.assertIn("try 4 of 4 failed", log)
        self.assertIn("Started a Release Dashboard run: opened #99 for main.", log)

    def test_the_job_may_dispatch_and_the_dashboard_can_be_dispatched(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split("\n  track:\n", 1)[1]
        permissions = job.split("    permissions:\n", 1)[1].split("    steps:\n", 1)[0]
        self.assertRegex(permissions, r"(?m)^      actions: write\b")
        # The dispatch names main, the only ref the dashboard publishes from.
        dashboard = WORKFLOWS / "release-dashboard.yml"
        self.assertIn("workflow_dispatch", workflow_triggers(dashboard))
        self.assertIn("github.ref == 'refs/heads/main'", dashboard.read_text(encoding="utf-8"))


class ReleaseDashboardWaitTests(unittest.TestCase):
    """#4604, #4620: a run that has something to add waits until the published data is five minutes old, and
    until a merge's second event has had time to arrive, whichever is longer, and no more. A manual run and a
    pull request's proof don't wait, and a manual run doesn't queue."""

    WORKFLOW = WORKFLOWS / "release-dashboard.yml"
    JOB = "generate"
    STEP = "Wait until the published data is five minutes old"
    NOW = 1_800_000_000
    NEVER_WAIT = {"workflow_dispatch", "pull_request"}
    # (how long ago the data was published, how long ago this run was created): the seconds the step sleeps.
    # None is "no dashboard-data branch yet" and "the run's start couldn't be read"; a negative age is a
    # runner whose clock is behind the commit's or GitHub's.
    CASES = {
        (None, None): None,
        (0, None): 300,
        (60, None): 240,
        (299, None): 1,
        (300, None): None,
        (301, None): None,
        (2220, None): None,
        (-3600, None): 300,
        # The second wait: read no sooner than SETTLE_SECONDS (15) after the run was created.
        (2220, 5): 10,
        (2220, 15): None,
        (2220, 20): None,
        (None, 5): 10,
        (60, 5): 240,  # the longer of the two
        (296, 5): 10,
        (2220, -100): 15,
    }
    ASKED = "asked about the budget"
    RESET = NOW + 900  # when the stand-in for GitHub says each budget's hour ends
    # #4717: after a wait the budget is asked about again. (how long ago the data was published, REST calls
    # left, GraphQL points left): whether the step lets the run go on. None is "the question failed".
    AFTER_THE_WAIT = {
        (60, 5000, 5000): True,
        (60, 100, 150): True,  # exactly the floors
        (60, 99, 5000): False,
        (60, 5000, 149): False,
        (60, 0, 0): False,
        (60, None, None): True,
        (2220, 0, 0): True,  # no wait, so nothing is asked: the step before it just did
    }

    @classmethod
    def setUpClass(cls):
        """Runs the step once per case in one bash process (each launch costs seconds on the ARM64 Windows
        runner, #3643). `date`, `sleep` and `gh` are functions, so the step's own arithmetic runs, nothing waits
        and the budget is what the case says."""
        cls.step = find_step(cls.WORKFLOW, cls.STEP, job=cls.JOB)
        tmp = Path(tempfile.mkdtemp(prefix="4604-dashboard-wait-"))
        cls.addClassCleanup(shutil.rmtree, tmp, ignore_errors=True)
        log = tmp / "log"
        script = [
            f"date() {{ echo {cls.NOW}; }}",
            'sleep() { echo "slept $1" >> "$LOG"; }',
            # Answers only the question the step should ask, as GitHub would for its filter: REST first.
            f'gh() {{ echo "{cls.ASKED}" >> "$LOG"; [[ "$1 $2 $3" == "api rate_limit --jq" && -n "$REST_LEFT" ]] || return 1;'
            f' echo "$REST_LEFT {cls.RESET} $POINTS_LEFT {cls.RESET}"; }}',
        ]

        def ago(seconds: int | None) -> str:
            return "" if seconds is None else str(cls.NOW - seconds)

        runs = [(published, created, 5000, 5000) for published, created in cls.CASES]
        runs += [(published, None, rest, points) for published, rest, points in cls.AFTER_THE_WAIT]
        for index, (published, created, rest, points) in enumerate(runs):
            # A subshell per case: the step's `set -e` and `exit` end the case, not the script.
            script += [
                f'echo "case {index}" >> "$LOG"',
                f'( export PUBLISHED_AT="{ago(published)}" CREATED_AT="{ago(created)}"',
                f'  export REST_LEFT="{"" if rest is None else rest}" POINTS_LEFT="{"" if points is None else points}"',
                f'  export GITHUB_OUTPUT="$DIR/output-{index}" GITHUB_STEP_SUMMARY="$DIR/summary-{index}"',
                cls.step.run,
                f') > "$DIR/stdout-{index}" 2>&1',
                'echo "status $?" >> "$LOG"',
            ]
        keys = ("MIN_AGE_SECONDS", "SETTLE_SECONDS", "MIN_REST_CALLS", "MIN_GRAPHQL_POINTS")
        env = {"LOG": posix(log), "DIR": posix(tmp), "GH_TOKEN": "none", **{key: cls.step.env[key] for key in keys}}
        cls.result = run_bash("\n".join(script), tmp, env)
        cls.seen: dict[int, list[str]] = {}
        case = -1
        for line in log.read_text(encoding="utf-8").splitlines() if log.is_file() else []:
            key, _, value = line.partition(" ")
            if key == "case":
                case = int(value)
                cls.seen[case] = []
            else:
                cls.seen[case].append(line)

        def read(kind: str, index: int) -> str:
            path = tmp / f"{kind}-{index}"
            return path.read_text(encoding="utf-8").strip() if path.is_file() else ""

        cls.wrote = {index: {kind: read(kind, index) for kind in ("output", "summary", "stdout")} for index in range(len(runs))}

    def test_the_minimum_age_is_five_minutes_and_the_settle_fifteen_seconds(self):
        self.assertEqual(self.step.env["MIN_AGE_SECONDS"], "300")
        self.assertEqual(self.step.env["SETTLE_SECONDS"], "15")

    def test_each_case_waits_what_it_should_and_no_more(self):
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        for index, (case, slept) in enumerate(self.CASES.items()):
            with self.subTest(published_ago=case[0], created_ago=case[1]):
                # The budget is asked about after a wait, and only then.
                expected = ([] if slept is None else [f"slept {slept}", self.ASKED]) + ["status 0"]
                self.assertEqual(self.seen[index], expected)
                self.assertEqual(self.wrote[index]["output"], "", "plenty of budget: the run goes on")

    def test_a_budget_spent_during_the_wait_stops_the_run_with_a_warning(self):
        # #4717: a run checked the budget, waited 50 seconds, and failed at its first read: a fuzzing run had
        # taken the last calls meanwhile. The floors are the ones the step before the wait holds the run to.
        gate = find_step(self.WORKFLOW, "Stop here when this run has nothing to add", job=self.JOB)
        for key in ("MIN_REST_CALLS", "MIN_GRAPHQL_POINTS"):
            self.assertEqual(self.step.env[key], gate.env[key], key)
        # The stub answers "REST, its reset, points, their reset": the step's filter must ask in that order.
        self.assertIn(
            r"""gh api rate_limit --jq '.resources | "\(.core.remaining) \(.core.reset) \(.graphql.remaining) \(.graphql.reset)"'""",
            self.step.run,
        )
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        for offset, ((published, rest, points), goes_on) in enumerate(self.AFTER_THE_WAIT.items()):
            index = len(self.CASES) + offset
            with self.subTest(published_ago=published, rest=rest, points=points):
                wrote = self.wrote[index]
                self.assertEqual(self.seen[index][-1], "status 0", wrote["stdout"])
                # A stopped run also hands on when the budget returns, for the page to say (#4726).
                stopped = sorted(["proceed=false", f"note={self.RESET}"])
                self.assertEqual(sorted(wrote["output"].splitlines()), [] if goes_on else stopped, wrote["stdout"])
                self.assertEqual(bool(wrote["summary"]), not goes_on)
                if not goes_on:
                    self.assertIn("::warning title=Release dashboard::The repository's API budget was used up during this run's wait",
                                  wrote["stdout"])
                    self.assertIn("Nothing read: the page is told when the budget returns.", wrote["summary"])
                self.assertEqual(self.ASKED in self.seen[index], published < 300, "asked only after a wait")
        text = self.WORKFLOW.read_text(encoding="utf-8")
        block = text.split(f"      - name: {self.STEP}\n", 1)[1].split("\n      - name: ", 1)[0]
        self.assertIn("        id: wait\n", block)

    def test_it_reads_the_times_the_steps_before_it_publish(self):
        self.assertEqual(self.step.env["PUBLISHED_AT"], "${{ steps.previous.outputs.at }}")
        self.assertEqual(self.step.env["CREATED_AT"], "${{ steps.gate.outputs.created }}")
        text = self.WORKFLOW.read_text(encoding="utf-8")
        fetch_block = text.split("      - name: Fetch the last published data\n", 1)[1].split("\n      - name: ", 1)[0]
        self.assertIn("        id: previous\n", fetch_block)
        fetch = find_step(self.WORKFLOW, "Fetch the last published data", job=self.JOB)
        self.assertIn('echo "at=$(git log -1 --format=%ct FETCH_HEAD)" >> "$GITHUB_OUTPUT"', fetch.run)

    def test_every_event_but_a_dispatch_and_a_pull_request_waits(self):
        # A new trigger joins the wait, or is added to NEVER_WAIT here on purpose. Only a run that has
        # something to add waits (ReleaseDashboardGateTests).
        triggers = set(workflow_triggers(self.WORKFLOW))
        self.assertTrue(self.NEVER_WAIT < triggers, triggers)
        listed = re.fullmatch(
            r"steps\.gate\.outputs\.proceed == 'true' && contains\(fromJSON\('(\[.*\])'\), github\.event_name\)",
            self.step.condition,
        )
        self.assertIsNotNone(listed, self.step.condition)
        self.assertEqual(set(json.loads(listed.group(1))), triggers - self.NEVER_WAIT)

    def test_the_job_outlasts_the_wait(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split("\n  generate:\n", 1)[1].split("\n  publish:\n", 1)[0]
        minutes = int(re.search(r"(?m)^    timeout-minutes: (\d+)", job).group(1))
        self.assertGreaterEqual(minutes * 60, int(self.step.env["MIN_AGE_SECONDS"]) + 600)

    def test_a_manual_run_cancels_the_run_in_its_way_and_the_others_queue(self):
        # "Refresh now" must not sit behind a run that is five minutes into nothing, or be replaced while it
        # waits. Every other publishing event queues, so a run that is publishing is never cut short by one.
        # Main Health's dispatch (#4700) comes from the workflow token and queues like the rest: if it
        # cancelled a run and then found the API budget gone, nothing would publish.
        text = self.WORKFLOW.read_text(encoding="utf-8")
        expression = re.search(r"(?m)^  cancel-in-progress: \$\{\{ (.+) \}\}$", text)
        self.assertIsNotNone(expression, "no cancel-in-progress expression")
        self.assertEqual(
            expression.group(1),
            "github.event_name == 'pull_request' || "
            "(github.event_name == 'workflow_dispatch' && github.actor != 'github-actions[bot]')",
        )
        # One group for everything that publishes; a pull request's runs are in a group of their own.
        self.assertEqual(concurrency_group(self.WORKFLOW),
                         "release-dashboard-${{ github.event_name == 'pull_request' && github.ref || 'publish' }}")


@unittest.skipUnless(shutil.which("jq"), "jq not available")
class ReleaseDashboardGateTests(unittest.TestCase):
    """#4620: a run with nothing to add stops before it waits or reads. That's a run when the token's REST calls
    or GraphQL points are gone (it could only fail, once for every event), and a run created before the
    published data's figures were read: the second run of a merge, and one that waited behind another."""

    WORKFLOW = WORKFLOWS / "release-dashboard.yml"
    JOB = "generate"
    STEP = "Stop here when this run has nothing to add"
    NOW = 1_800_000_000
    RESET_IN = 600  # each budget's hour ends in ten minutes
    GOES_ON = "true"
    STOPS = "false"
    MAIN_HEALTH = "workflow_dispatch from Main Health"  # sent with the workflow token (#4700)
    # name: (event, how long ago the data was published, how long ago its figures were read, REST calls left,
    #        GraphQL points left, how long ago this run was created, what the step decides). Seconds. None is
    #        "nothing published", "index.json doesn't say", "the budget question failed" (both budgets) and
    #        "this run couldn't be read".
    CASES = {
        "a manual run the data already has": ("workflow_dispatch", 20, 60, 5000, 5000, 120, GOES_ON),
        "Main Health's dispatch, main's new state already in the data": (MAIN_HEALTH, 20, 60, 5000, 5000, 120, STOPS),
        "Main Health's dispatch, not in the data yet": (MAIN_HEALTH, 20, 60, 5000, 5000, 30, GOES_ON),
        "Main Health's dispatch with the REST calls gone": (MAIN_HEALTH, 20, 60, 99, 5000, 30, STOPS),
        "a pull request with no budget left": ("pull_request", 60, 100, 0, 0, 30, GOES_ON),
        "a manual run": ("workflow_dispatch", 60, 100, 5000, 5000, 30, GOES_ON),
        "a manual run with the REST calls gone": ("workflow_dispatch", 60, 100, 99, 5000, 30, STOPS),
        "the schedule with the GraphQL points gone": ("schedule", 900, 940, 5000, 149, 30, STOPS),
        "an issue with exactly enough of both": ("issues", 20, 60, 100, 150, 10, GOES_ON),
        "the schedule, nothing published yet": ("schedule", None, None, 5000, 5000, 30, GOES_ON),
        "an issue the data was read 10 s after": ("issues", 20, 60, 5000, 5000, 70, STOPS),
        "an issue the data was read 9 s after": ("issues", 20, 60, 5000, 5000, 69, GOES_ON),
        "a push after the data was read": ("push", 20, 60, 5000, 5000, 30, GOES_ON),
        "the schedule that waited behind the run that read": ("schedule", 20, 60, 5000, 5000, 120, STOPS),
        "the schedule, created after the data was read": ("schedule", 370, 410, 5000, 5000, 10, GOES_ON),
        "a push whose run can't be read": ("push", 20, 60, 5000, 5000, None, GOES_ON),
        "an issue, the data not saying when it was read": ("issues", 20, None, 5000, 5000, 70, GOES_ON),
        "an issue in the data, the budget question failing": ("issues", 20, 60, None, None, 70, STOPS),
        # #4726: a spent budget is said on the page, once for each hour it is gone.
        "the schedule, the REST calls gone and the page told already": ("schedule", 300, 900, 0, 5000, 30, STOPS),
        "the schedule, the REST calls gone and the page told of an earlier hour": ("schedule", 300, 900, 0, 5000, 30, STOPS),
        "the schedule, the REST calls gone and nothing published": ("schedule", None, None, 0, 5000, 30, STOPS),
    }
    # What the published index.json already says of the budget, in seconds from the end of this hour.
    TOLD = {
        "the schedule, the REST calls gone and the page told already": 0,
        "the schedule, the REST calls gone and the page told of an earlier hour": -3600,
    }

    @classmethod
    def setUpClass(cls):
        """Every case in one bash process, each in a subshell. `date +%s` and `gh` are functions: the clock
        stands at NOW, and `gh api` answers the budgets and this run's creation time from the case's
        environment. jq and `date -d` are the real ones, on an index.json written for the case."""
        cls.step = find_step(cls.WORKFLOW, cls.STEP, job=cls.JOB)
        tmp = Path(tempfile.mkdtemp(prefix="4620-dashboard-gate-"))
        cls.addClassCleanup(shutil.rmtree, tmp, ignore_errors=True)
        reset = cls.NOW + cls.RESET_IN
        script = [
            f'date() {{ if [[ "$1" == "+%s" ]]; then echo {cls.NOW}; else command date "$@"; fi; }}',
            "gh() {",
            '  if [[ "$2" == rate_limit ]]; then',
            '    [[ -n "$REST_LEFT" ]] || return 1',
            f'    echo "$REST_LEFT {reset} $POINTS_LEFT {reset}"',
            "  else",
            '    [[ -n "$CREATED" ]] || return 1',
            '    echo "$CREATED"',
            "  fi",
            "}",
        ]

        def ago(seconds: int | None) -> str:
            return "" if seconds is None else str(cls.NOW - seconds)

        def stamp_of(seconds: int) -> str:
            return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(seconds))

        for index, (name, (event, published, read, rest, points, created, _)) in enumerate(cls.CASES.items()):
            if published is not None:
                stamp = {} if read is None else {"generated_at": stamp_of(cls.NOW - read)}
                if name in cls.TOLD:
                    stamp["budget_until"] = stamp_of(reset + cls.TOLD[name])
                (tmp / f"index-{index}.json").write_text(json.dumps({"schema": 1, **stamp}), encoding="utf-8")
            actor = "github-actions[bot]" if event == cls.MAIN_HEALTH else "a-person"
            script += [
                f'( export EVENT="{event.split()[0]}" ACTOR="{actor}"',
                f'  export PUBLISHED_AT="{ago(published)}" PREVIOUS_INDEX="$DIR/index-{index}.json"',
                f'  export REST_LEFT="{"" if rest is None else rest}" POINTS_LEFT="{"" if points is None else points}"',
                f'  export CREATED="{ago(created)}"',
                f'  export GITHUB_OUTPUT="$DIR/output-{index}" GITHUB_STEP_SUMMARY="$DIR/summary-{index}"',
                cls.step.run,
                f') > "$DIR/log-{index}" 2>&1',
                f'echo "$?" > "$DIR/status-{index}"',
            ]
        env = {key: cls.step.env[key] for key in ("MIN_REST_CALLS", "MIN_GRAPHQL_POINTS", "MARGIN_SECONDS")}
        env.update({"DIR": posix(tmp), "RUN_PATH": "repos/owner/repo/actions/runs/1", "GH_TOKEN": "none"})
        cls.result = run_bash("\n".join(script), tmp, env)

        def text(kind: str, index: int) -> str:
            path = tmp / f"{kind}-{index}"
            return path.read_text(encoding="utf-8").strip() if path.is_file() else ""

        cls.seen = {}
        for index, name in enumerate(cls.CASES):
            seen = {kind: text(kind, index) for kind in ("summary", "log", "status")}
            seen["output"] = dict(line.split("=", 1) for line in text("output", index).splitlines() if "=" in line)
            cls.seen[name] = seen

    def test_each_case_goes_on_or_stops_as_it_should(self):
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        for name, (*_, decision) in self.CASES.items():
            with self.subTest(case=name):
                seen = self.seen[name]
                self.assertEqual(seen["status"], "0", seen["log"])
                self.assertEqual(seen["output"].get("proceed"), decision, seen["log"])

    def test_a_spent_budget_is_a_warning_that_says_which_and_for_how_long(self):
        for name, which in (("a manual run with the REST calls gone", "99 REST calls left"),
                            ("the schedule with the GraphQL points gone", "149 GraphQL points left")):
            with self.subTest(case=name):
                log = self.seen[name]["log"]
                self.assertIn(f"::warning title=Release dashboard::The repository's API budget is used up: {which}", log)
                self.assertIn("for about 10 more minutes", log)
                self.assertIn("Nothing read: the page is told when the budget returns.", self.seen[name]["summary"])

    def test_a_spent_budget_is_handed_on_for_the_page_once_for_each_hour(self):
        # #4726: the run that finds the budget gone hands on when it returns, and the last data is published
        # again with that time. The runs after it in the same hour find the page told, and publish nothing.
        reset = str(self.NOW + self.RESET_IN)
        handed_on = {
            "a manual run with the REST calls gone": reset,
            "the schedule with the GraphQL points gone": reset,
            "Main Health's dispatch with the REST calls gone": reset,
            "the schedule, the REST calls gone and the page told of an earlier hour": reset,
            "the schedule, the REST calls gone and the page told already": None,
            "the schedule, the REST calls gone and nothing published": None,
        }
        for name, note in handed_on.items():
            with self.subTest(case=name):
                seen = self.seen[name]
                self.assertEqual(seen["output"].get("note"), note, seen["log"])
                said = "Nothing read: the page is told when the budget returns." if note else "Nothing read, nothing published."
                self.assertIn(said, seen["summary"])
                self.assertEqual(seen["log"].count("::warning title=Release dashboard::"), 1, seen["log"])
        # No other case hands anything on: a run that goes on, or one the data already covers.
        for name in set(self.CASES) - set(handed_on):
            with self.subTest(case=name):
                self.assertNotIn("note", self.seen[name]["output"], self.seen[name]["log"])

    def test_a_run_that_stops_says_why_in_its_summary_and_one_that_goes_on_writes_none(self):
        for name, (*_, decision) in self.CASES.items():
            with self.subTest(case=name):
                self.assertEqual(bool(self.seen[name]["summary"]), decision == self.STOPS, self.seen[name]["log"])
        self.assertIn("read 10 s after this run was created", self.seen["an issue the data was read 10 s after"]["summary"])
        self.assertIn("read 60 s after this run was created",
                      self.seen["the schedule that waited behind the run that read"]["summary"])

    def test_a_run_that_goes_on_hands_the_wait_its_own_start(self):
        # The wait step reads `created` to give a merge's second event time to arrive.
        self.assertEqual(self.seen["a push after the data was read"]["output"].get("created"), str(self.NOW - 30))
        self.assertEqual(self.seen["an issue, the data not saying when it was read"]["output"].get("created"), str(self.NOW - 70))
        for name in ("a push whose run can't be read", "a manual run", "a pull request with no budget left"):
            with self.subTest(case=name):
                self.assertNotIn("created", self.seen[name]["output"])

    def test_a_person_and_main_health_are_told_apart_by_who_sent_the_dispatch(self):
        self.assertEqual(self.step.env["ACTOR"], "${{ github.actor }}")
        self.assertIn("A manual run always generates.", self.seen["a manual run the data already has"]["log"])
        self.assertIn("it has everything this run would add",
                      self.seen["Main Health's dispatch, main's new state already in the data"]["summary"])
        # Main Health's run isn't in the wait step's list: main turning red or green doesn't wait five minutes.
        wait = find_step(self.WORKFLOW, ReleaseDashboardWaitTests.STEP, job=self.JOB)
        self.assertNotIn("workflow_dispatch", wait.condition)

    def test_a_merges_second_event_is_in_the_first_runs_data_by_more_than_the_margin(self):
        # A push and an issue closing arrive about two seconds apart. The first run reads SETTLE_SECONDS after
        # it was created at the earliest, so that read has to clear the second event by MARGIN_SECONDS for the
        # second run to stop; five seconds of slack covers events further apart than usual.
        wait = find_step(self.WORKFLOW, ReleaseDashboardWaitTests.STEP, job=self.JOB)
        self.assertEqual(self.step.env["PUBLISHED_AT"], "${{ steps.previous.outputs.at }}")
        self.assertEqual(self.step.env["PREVIOUS_INDEX"], "${{ runner.temp }}/previous/index.json")
        self.assertGreaterEqual(int(wait.env["SETTLE_SECONDS"]) - int(self.step.env["MARGIN_SECONDS"]), 5)
        # The margin is what the two clocks may disagree by: the generator's runner stamps the read.
        self.assertGreaterEqual(int(self.step.env["MARGIN_SECONDS"]), 10)

    def test_nothing_is_read_or_published_after_a_stop(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        gate = text.split(f"      - name: {self.STEP}\n", 1)[1].split("\n      - name: ", 1)[0]
        self.assertIn("        id: gate\n", gate)
        # Two steps can stop a run: this one, and the wait when the budget went during it (#4717).
        go_on = "steps.gate.outputs.proceed == 'true' && steps.wait.outputs.proceed != 'false'"
        self.assertEqual(find_step(self.WORKFLOW, "Generate", job=self.JOB).condition, go_on)
        # What a stopped run may still publish is the note that the budget is gone (#4726): the last data
        # again, read from nowhere. The check and the pack run for it as for data, and nothing else does.
        has_data = f"({go_on}) || steps.note.outcome == 'success'"
        for name in ("Check what was written", "Pack the data for the publish job"):
            with self.subTest(step=name):
                self.assertEqual(find_step(self.WORKFLOW, name, job=self.JOB).condition, has_data)
        generate = text.split("\n  generate:\n", 1)[1].split("\n  publish:\n", 1)[0]
        self.assertIn("    outputs:\n", generate)
        self.assertIn(f"      publish: ${{{{ {has_data} }}}}\n", generate)
        publish = text.split("\n  publish:\n", 1)[1]
        condition = publish.split("    if: >-\n", 1)[1].split("\n    runs-on:", 1)[0]
        self.assertIn("needs.generate.outputs.publish == 'true'", condition)
        self.assertIn("github.event_name != 'pull_request'", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)

    def test_only_the_events_worth_a_run_start_one(self):
        # Each event starts a run, and a run that isn't needed shows as cancelled or stops here: Main Health
        # was two runs in three, and a new issue fired one `labeled` for each of its labels.
        triggers = workflow_triggers(self.WORKFLOW)
        self.assertEqual(set(triggers), {"schedule", "workflow_dispatch", "issues", "push", "pull_request"})
        # No `opened` (#4700): the page shows only issues on a milestone, and one created with a milestone
        # fires `milestoned` too, so both together were two runs for every new issue.
        self.assertEqual(_list_value(*triggers["issues"]["types"]),
                         ["closed", "reopened", "deleted", "milestoned", "demilestoned"])
        self.assertEqual(_list_value(*triggers["push"]["branches"]), ["main"])

    def test_the_schedule_has_two_lines_five_minutes_apart(self):
        # #4700: the hour's first slot starts late or not at all, and one line alone left the page 15 to 21
        # minutes behind in a quiet stretch. Five minutes is also the shortest interval GitHub schedules.
        text = self.WORKFLOW.read_text(encoding="utf-8")
        lines = re.findall(r"(?m)^    - cron: '(\d+)-59/10 \* \* \* \*'$", text)
        self.assertEqual(lines, ["3", "8"])
        self.assertEqual(len(re.findall(r"(?m)^    - cron: ", text)), 2)


class ReleaseDashboardBudgetNoteTests(unittest.TestCase):
    """#4726: when the API budget is gone the page says so, and when it returns. The step that stopped the run
    hands on that time, and "Tell the page when the budget returns" writes the last data again with it, by the
    generator's --budget-until. No API call: the data came by git and goes back by git."""

    WORKFLOW = WORKFLOWS / "release-dashboard.yml"
    JOB = "generate"
    STEP = "Tell the page when the budget returns"
    FIXTURES = REPO / "tests" / "tools" / "fixtures" / "release-dashboard"
    UNTIL = 1_800_003_600

    def test_it_runs_for_either_steps_note_and_only_with_data_to_repeat(self):
        step = find_step(self.WORKFLOW, self.STEP, job=self.JOB)
        self.assertEqual(step.condition, "(steps.gate.outputs.note != '' || steps.wait.outputs.note != '') && steps.previous.outputs.at != ''")
        # The wait's note is the later look at the budget, so it wins.
        self.assertEqual(step.env["UNTIL"], "${{ steps.wait.outputs.note || steps.gate.outputs.note }}")
        self.assertEqual(
            step.run.strip(),
            'python3 -I -X utf8 tools/release-dashboard.py --out "$RUNNER_TEMP/data" --previous "$RUNNER_TEMP/previous" --budget-until "$UNTIL"',
        )
        text = self.WORKFLOW.read_text(encoding="utf-8")
        generate = text.split("\n  generate:\n", 1)[1].split("\n  publish:\n", 1)[0]
        block = generate.split(f"      - name: {self.STEP}\n", 1)[1].split("\n      - name: ", 1)[0]
        self.assertIn("        id: note\n", block)
        # It needs no token: nothing in it may reach for the API.
        self.assertNotIn("GH_TOKEN", block)
        names = step_names(self.WORKFLOW, job=self.JOB)
        self.assertLess(names.index("Wait until the published data is five minutes old"), names.index(self.STEP))
        self.assertLess(names.index(self.STEP), names.index("Check what was written"))

    def test_what_it_writes_passes_the_workflows_own_check(self):
        # The note's output goes through "Check what was written" like generated data. Run both for real:
        # the generator on the committed fixtures as the last data, then the check step's own script.
        check = find_step(self.WORKFLOW, "Check what was written", job=self.JOB)
        script = check.run.split("<<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
        with tempfile.TemporaryDirectory(prefix="4726-budget-note-") as out:
            wrote = subprocess.run(
                [sys.executable, "-I", "-X", "utf8", str(REPO / "tools" / "release-dashboard.py"), "--out", out,
                 "--previous", str(self.FIXTURES), "--budget-until", str(self.UNTIL)],
                capture_output=True, text=True, encoding="utf-8", timeout=60, check=False,
            )
            self.assertEqual(wrote.returncode, 0, wrote.stdout + wrote.stderr)
            index = json.loads((Path(out) / "index.json").read_text(encoding="utf-8"))
            before = json.loads((self.FIXTURES / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["budget_until"], time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self.UNTIL)))
            self.assertEqual(index["generated_at"], before["generated_at"])
            checked = subprocess.run(
                [sys.executable, "-I", "-X", "utf8", "-", out], input=script,
                capture_output=True, text=True, encoding="utf-8", timeout=60, check=False,
            )
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertIn(f"current {before['current']}", checked.stdout)
            # Flat JSON with an index.json, which is all the publish job accepts.
            self.assertEqual({path.suffix for path in Path(out).iterdir()}, {".json"})


@unittest.skipUnless(bash_has_jq(), "jq is not on bash's PATH")
class ReleaseDashboardHandoverTests(unittest.TestCase):
    """#4717: the generate job hands its JSON to the publish job as a job output, not as an artifact (one a run
    was about 290 a day in the list that every ClusterFuzzLite lookup pages through, #4617). The publish job
    holds the token that can write, so it takes the string as text that can't become commands, refuses anything
    but a flat set of JSON files, and tries its push three times."""

    # What a generate job that had been taken over could hand on: a line break, the here-document's end marker,
    # and a command. Pasted into the script as it is, the command would run in the job that can write.
    INJECTED = "AAAA\nPACKED\ntouch \"$DIR/injected\"\ncat <<'PACKED'"

    WORKFLOW = WORKFLOWS / "release-dashboard.yml"
    FILES = {
        "index.json": json.dumps({"schema": 4, "current": "v1.1.0", "releases": [{"title": "v1.1.0"}]}),
        "v1.1.0.json": json.dumps({"milestone": {"title": "v1.1.0"}, "note": "an em dash — and <b>markup</b> stay as written"}),
    }

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="4717-dashboard-handover-"))
        cls.addClassCleanup(shutil.rmtree, cls.tmp, ignore_errors=True)
        cls.pack = find_step(cls.WORKFLOW, "Pack the data for the publish job", job="generate")
        cls.unpack = find_step(cls.WORKFLOW, "Unpack the data", job="publish")
        cls.push = find_step(cls.WORKFLOW, "Replace the branch's one commit", job="publish")

        # One launch: the generate job's pack step, on real files, as it is and with a limit it can't meet.
        data = cls.tmp / "runner" / "data"
        data.mkdir(parents=True)
        for name, body in cls.FILES.items():
            (data / name).write_text(body, encoding="utf-8", newline="\n")
        script = [
            '( export GITHUB_OUTPUT="$DIR/pack-output"', cls.pack.run, ') > "$DIR/pack-log" 2>&1',
            'echo "$?" > "$DIR/pack-status"',
            '( export GITHUB_OUTPUT="$DIR/small-output" MAX_CHARACTERS=10', cls.pack.run, ') > "$DIR/small-log" 2>&1',
            'echo "$?" > "$DIR/small-status"',
        ]
        env = {"DIR": posix(cls.tmp), "RUNNER_TEMP": posix(cls.tmp / "runner"), "MAX_CHARACTERS": cls.pack.env["MAX_CHARACTERS"]}
        cls.packing = run_bash("\n".join(script), cls.tmp, env)
        output = cls.read("pack-output")
        cls.packed = output.partition("=")[2] if output.startswith("data=") else ""

        # A second launch: the publish job's unpack step on that string, and on three it must refuse.
        def packed(files: dict[str, str], folder: str = "") -> str:
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
                if folder:
                    entry = tarfile.TarInfo(folder)
                    entry.type = tarfile.DIRTYPE
                    entry.mode = 0o755
                    archive.addfile(entry)
                for name, body in files.items():
                    raw = body.encode("utf-8")
                    entry = tarfile.TarInfo(f"{folder}/{name}" if folder else name)
                    entry.size = len(raw)
                    archive.addfile(entry, io.BytesIO(raw))
            return base64.b64encode(buffer.getvalue()).decode("ascii")

        cls.handed = {
            "what the pack step made": cls.packed,
            "a script beside the JSON": packed({**cls.FILES, "run-me.sh": "echo hello"}),
            "the JSON inside a folder": packed(cls.FILES, folder="nested"),
            "no index.json": packed({"v1.1.0.json": cls.FILES["v1.1.0.json"]}),
            "a line break, the end marker and a command": cls.INJECTED,
            "nothing": "",
            "two lines of base64": cls.packed[:40] + "\n" + cls.packed[40:80],
            "base64 that isn't an archive": "AAAA",
        }
        script = []
        for index, data_string in enumerate(cls.handed.values()):
            (cls.tmp / f"unpack-{index}").mkdir()
            script += [
                f'( cd "$DIR/unpack-{index}"',
                # GitHub's toJSON() of a string is the string as JSON, on one line: json.dumps is the same.
                substitute(cls.unpack.run, {"toJSON(needs.generate.outputs.data)": json.dumps(data_string)}),
                f') > "$DIR/unpack-log-{index}" 2>&1',
                f'echo "$?" > "$DIR/unpack-status-{index}"',
            ]
        cls.unpacking = run_bash("\n".join(script), cls.tmp, {"DIR": posix(cls.tmp)})

        # A third: the push, with `git` failing as often as the case says. `timeout` and `sleep` only log.
        script = [
            "git() {",
            '  if [[ " $* " == *" --amend "* ]]; then echo "commit again" >> "$CALLS"; fi',
            '  if [[ " $* " != *" push "* ]]; then return 0; fi',
            '  echo "push" >> "$CALLS"',
            '  (( $(grep -c "^push$" "$CALLS") > FAILING_PUSHES ))',
            "}",
            'timeout() { echo "limit $1" >> "$CALLS"; shift; "$@"; }',
            'sleep() { echo "slept $1" >> "$CALLS"; }',
        ]
        cls.push_cases = {"the first push works": 0, "the third push works": 2, "no push works": 3}
        for index, failing in enumerate(cls.push_cases.values()):
            folder = cls.tmp / f"push-{index}" / "data"
            folder.mkdir(parents=True)
            (folder / "index.json").write_text("{}", encoding="utf-8")
            script += [
                f'( cd "$DIR/push-{index}"; export FAILING_PUSHES={failing} CALLS="$DIR/push-calls-{index}"',
                f'  export GITHUB_STEP_SUMMARY="$DIR/push-summary-{index}"; : > "$CALLS"',
                cls.push.run,
                f') > "$DIR/push-log-{index}" 2>&1',
                f'echo "$?" > "$DIR/push-status-{index}"',
            ]
        env = {
            "DIR": posix(cls.tmp),
            "GH_TOKEN": "none",
            "REPO_URL": "https://github.example/owner/repo.git",
            "SOURCE": "https://github.example/owner/repo",
            "RUN_URL": "https://github.example/owner/repo/actions/runs/42",
            "GITHUB_SHA": "0123456789abcdef0123456789abcdef01234567",
            "GITHUB_RUN_ID": "42",
        }
        cls.pushing = run_bash("\n".join(script), cls.tmp, env)

    @classmethod
    def read(cls, name: str) -> str:
        path = cls.tmp / name
        return path.read_text(encoding="utf-8").strip() if path.is_file() else ""

    def test_the_data_is_one_line_of_base64_under_the_limit(self):
        self.assertEqual(self.packing.returncode, 0, self.packing.stdout + self.packing.stderr)
        self.assertEqual(self.read("pack-status"), "0", self.read("pack-log"))
        self.assertRegex(self.packed, r"\A[A-Za-z0-9+/]+=*\Z")
        self.assertIn("Packed 2 files into", self.read("pack-log"))
        # A job's outputs hold a megabyte; whether that's characters or bytes of a wider encoding isn't
        # written down, so the ceiling stays under half of it.
        self.assertLessEqual(int(self.pack.env["MAX_CHARACTERS"]), 500_000)

    def test_data_too_large_for_an_output_fails_the_run_and_hands_over_nothing(self):
        self.assertEqual(self.read("small-status"), "1", self.read("small-log"))
        self.assertIn("::error title=Release dashboard::The data is", self.read("small-log"))
        self.assertEqual(self.read("small-output"), "")

    def test_what_generate_packs_is_what_publish_unpacks(self):
        self.assertEqual(self.unpacking.returncode, 0, self.unpacking.stdout + self.unpacking.stderr)
        self.assertEqual(self.read("unpack-status-0"), "0", self.read("unpack-log-0"))
        folder = self.tmp / "unpack-0" / "data"
        self.assertEqual(sorted(path.name for path in folder.iterdir()), sorted(self.FILES))
        for name, body in self.FILES.items():
            with self.subTest(file=name):
                self.assertEqual((folder / name).read_text(encoding="utf-8"), body)
        for left_over in ("data.tgz", "data.b64"):
            self.assertFalse((self.tmp / "unpack-0" / left_over).exists(), left_over)
        self.assertIn("Unpacked 2 files.", self.read("unpack-log-0"))

    def test_publish_refuses_anything_but_a_flat_set_of_json_files_with_an_index(self):
        for index, name in enumerate(self.handed):
            if name not in ("a script beside the JSON", "the JSON inside a folder", "no index.json"):
                continue
            with self.subTest(handed=name):
                self.assertEqual(self.read(f"unpack-status-{index}"), "1", self.read(f"unpack-log-{index}"))
                self.assertIn("isn't a flat set of JSON files with an index.json", self.read(f"unpack-log-{index}"))

    def test_a_string_that_isnt_one_line_of_base64_is_refused_and_never_runs(self):
        # The review's finding: pasted in as it was, a string with a line break and the here-document's end
        # marker ran its next line as a command, in the job that can write. As JSON it is one line of text.
        self.assertFalse((self.tmp / "injected").exists(), "the handed-over string ran as a command")
        # Each gets the step's own error, not a raw one from gzip or tar. GitHub hands over nothing when an
        # output is too large or looks to it like a secret.
        says = {
            "a line break, the end marker and a command": "characters that aren't one line of base64. Nothing is published.",
            "two lines of base64": "characters that aren't one line of base64. Nothing is published.",
            "nothing": "The generate job handed over no data:",
            "base64 that isn't an archive": "What the generate job handed over doesn't unpack. Nothing is published.",
        }
        for index, name in enumerate(self.handed):
            if name not in says:
                continue
            with self.subTest(handed=name):
                log = self.read(f"unpack-log-{index}")
                self.assertEqual(self.read(f"unpack-status-{index}"), "1", log)
                self.assertIn("::error title=Release dashboard::", log)
                self.assertIn(says[name], log)
                self.assertFalse((self.tmp / f"unpack-{index}" / "data" / "index.json").exists(), "nothing is unpacked")
        self.assertIn("${{ toJSON(needs.generate.outputs.data) }}", self.unpack.run)
        self.assertNotIn("${{ needs.generate.outputs.data }}", self.WORKFLOW.read_text(encoding="utf-8"))
        text = self.WORKFLOW.read_text(encoding="utf-8")
        generate = text.split("\n  generate:\n", 1)[1].split("\n  publish:\n", 1)[0]
        self.assertIn("      data: ${{ steps.pack.outputs.data }}\n", generate)
        self.assertIn("        id: pack\n", generate)

    def test_only_a_run_that_cant_publish_keeps_an_artifact(self):
        # A pull request's run and a manual run on a branch publish nothing, so their JSON is kept for the
        # reviewer. A run on main hands the data over without one, and the publish job downloads none.
        text = self.WORKFLOW.read_text(encoding="utf-8")
        uses = [action.split("@")[0] for action in re.findall(r"(?m)^\s+(?:- )?uses: (\S+)", text)]
        self.assertEqual(uses, ["actions/checkout", "actions/upload-artifact"], uses)
        generate = text.split("\n  generate:\n", 1)[1].split("\n  publish:\n", 1)[0]
        keep = generate.split("      - name: Keep the data of a run that can't publish\n", 1)[1]
        condition = " ".join(keep.split("        if: >-\n", 1)[1].split("\n        uses:", 1)[0].split())
        # The opposite of the publish job's own condition, and only when there is data to keep.
        self.assertEqual(condition, "(github.event_name == 'pull_request' || github.ref != 'refs/heads/main') && "
                                    "steps.gate.outputs.proceed == 'true' && steps.wait.outputs.proceed != 'false'")
        publish = text.split("\n  publish:\n", 1)[1]
        self.assertIn("github.event_name != 'pull_request' && github.ref == 'refs/heads/main'", publish)
        self.assertNotIn("uses:", publish)

    def test_a_push_is_tried_three_times_a_minute_each_before_the_run_fails(self):
        self.assertEqual(self.pushing.returncode, 0, self.pushing.stdout + self.pushing.stderr)
        # Before each later try the commit is made again, so its time is the push's: the next run's five-minute
        # wait is measured from it.
        three = ["limit 60", "push", "slept 15", "commit again", "limit 60", "push", "slept 45", "commit again", "limit 60", "push"]
        expected = {
            "the first push works": ("0", ["limit 60", "push"]),
            "the third push works": ("0", three),
            "no push works": ("1", three),
        }
        for index, name in enumerate(self.push_cases):
            with self.subTest(case=name):
                status, calls = expected[name]
                log = self.read(f"push-log-{index}")
                self.assertEqual(self.read(f"push-status-{index}"), status, log)
                self.assertEqual(self.read(f"push-calls-{index}").splitlines(), calls, log)
                published = "Published 1 files to dashboard-data." in self.read(f"push-summary-{index}")
                self.assertEqual(published, status == "0")
                self.assertEqual("The push to dashboard-data failed three times." in log, status == "1")

    def test_the_publish_job_outlasts_three_pushes(self):
        text = self.WORKFLOW.read_text(encoding="utf-8")
        job = text.split("\n  publish:\n", 1)[1]
        minutes = int(re.search(r"(?m)^    timeout-minutes: (\d+)", job).group(1))
        self.assertGreaterEqual(minutes * 60, 3 * 60 + 15 + 45 + 120)


if __name__ == "__main__":
    # With no --only-/--skip- flag every case runs, as before.
    only = [arg[len("--only-"):] for arg in sys.argv[1:] if arg.startswith("--only-") and arg[len("--only-"):] in GROUPS]
    skip = [arg[len("--skip-"):] for arg in sys.argv[1:] if arg.startswith("--skip-") and arg[len("--skip-"):] in GROUPS]
    if only or skip:
        sys.argv = [arg for arg in sys.argv if arg not in {f"--only-{g}" for g in only} | {f"--skip-{g}" for g in skip}]
        loaded = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])

        def keep(test: unittest.TestCase) -> bool:
            if only:
                return any(GROUPS[group](test) for group in only)
            return not any(GROUPS[group](test) for group in skip)

        result = unittest.TextTestRunner(verbosity=1).run(_filtered(loaded, keep))
        # A split that runs nothing (a renamed class, a group that no longer matches) fails rather than passing (#3841).
        if result.testsRun == 0:
            print(f"no test ran for {sys.argv[1:] or 'the groups asked for'}: --only-/--skip- matched nothing", file=sys.stderr)
            sys.exit(1)
        sys.exit(0 if result.wasSuccessful() else 1)
    unittest.main()
