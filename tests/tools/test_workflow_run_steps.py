#!/usr/bin/env python3
"""Runs the shell of workflow steps whose bugs lived only inside a `run:` block (#2478).

- renovate.yml "Select dry-run mode": a scheduled or `disabled` run must not set RENOVATE_DRY_RUN,
  which Renovate reads as a dry run whatever its value (#2527).
- sanitizers.yml "Add sanitizer summary to workflow summary": the report's file name is printed in
  Markdown code marks, not run as a command substitution (#2532).
- main-health.yml "Open, update or close the tracking issue": a job only some runs execute (CI's
  weekly unity build) stays red until a run that ran it passes (#2313). `gh` is a stub that answers
  from fixtures through the step's own jq filters; only the step's `judge_branch` function runs,
  cut before the issue handling that follows it, for main (#2741 tracks release branches the same way).
- release.yml verify-reproducibility-windows "Compare digests": matching packages pass whatever the
  build-inputs manifests say, and only differing packages fail (#2952). Run with pwsh the way GitHub's
  `shell: pwsh` wrapper runs it, against fixture .zips and manifests and the real comparator script.
- codeql.yml "Keep results under src/": a result is kept when any of its locations, code-flow steps
  or related locations is under src/, so a taint flow from src/ into a third-party sink survives;
  only results with no src/ location at all are dropped (#2992).

Each step is read from the workflow file itself (tests/tools/workflow_steps.py), its `${{ }}`
expressions replaced by fixed values, and run with bash (pwsh for the pwsh step). Needs bash; the
Main Health and CodeQL cases also need jq, and the reproducibility cases pwsh, and are skipped
without them.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from workflow_steps import find_step, substitute  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
