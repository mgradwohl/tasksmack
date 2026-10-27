#!/usr/bin/env python3
"""Tests for tools/ci-change-classifier.py, which decides the C++ jobs a pull request needs (#3104).

- The classification table: C++ sources and headers, tests, benchmarks, assets, CMake, presets and
  toolchains, dependency declarations, build and tidy helpers, the reusable workflow and the local
  actions, docs, every automation file, mixed and unknown paths, and the classifier itself.
- git diff input: modified, deleted and renamed paths, an empty change and a failed git diff.
- The allowlist's review rules: every automation file exists, has a reason, and no input of a
  skipped C++ job names one; a tests/tools script that names one is run by ci.yml's automation-checks.
- ci.yml's side of the contract: the detect job's outputs and SAFE_OUTPUTS match the classifier's.
- ci.yml's "Classify the changes" step, run with bash on throwaway merge commits, and the CI Success
  "Check all required jobs passed" step, run with bash on job results: an expected skip passes, and a
  failed classification, an unexpected skip, a cancellation or a failed job never does.

The two step tests need bash (Git for Windows' on Windows) and are skipped without it.
"""

from __future__ import annotations

import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from workflow_steps import find_step  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "ci-change-classifier.py"
CI = REPO / ".github" / "workflows" / "ci.yml"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash")

_spec = importlib.util.spec_from_file_location("ci_change_classifier", SCRIPT)
classifier = importlib.util.module_from_spec(_spec)
sys.modules["ci_change_classifier"] = classifier  # dataclasses looks the module up by name
_spec.loader.exec_module(classifier)

NAMES = [output.name for output in classifier.OUTPUTS]
SAFE = {name: False for name in NAMES}


def expect(**true_flags: bool) -> dict[str, bool]:
    values = dict(SAFE)
    values.update(true_flags)
    return values


DOCS_ONLY = expect(docs_only=True)
LINUX_ONLY = expect(linux_only=True)
WINDOWS_ONLY = expect(windows_only=True)
AUTOMATION_ONLY = expect(automation_only=True)

# (what the case covers, changed paths, expected outputs)
CASES = [
    # C++ sources and headers
    ("shared source", ["src/Core/Application.cpp"], SAFE),
    ("shared header", ["src/Core/Application.h"], SAFE),
    ("configure template", ["src/Core/Version.h.in"], SAFE),
    ("Linux platform source", ["src/Platform/Linux/LinuxProcessProbe.cpp"], LINUX_ONLY),
    ("nested Linux platform source", ["src/Platform/Linux/Detail/Parser.cpp"], LINUX_ONLY),
    ("Linux platform header (built into the shared tests)", ["src/Platform/Linux/LinuxOsInfo.h"], SAFE),
    ("Windows platform source", ["src/Platform/Windows/WindowsProcessProbe.cpp"], WINDOWS_ONLY),
    ("Windows platform header (built on Linux)", ["src/Platform/Windows/WindowsMath.h"], SAFE),
    ("Linux and Windows sources", ["src/Platform/Linux/A.cpp", "src/Platform/Windows/B.cpp"], SAFE),
    ("Linux source and a shared header", ["src/Platform/Linux/A.cpp", "src/Core/A.h"], SAFE),
    ("Linux source and docs", ["src/Platform/Linux/A.cpp", "README.md"], SAFE),
    ("Linux source and automation", ["src/Platform/Linux/A.cpp", ".github/workflows/renovate.yml"], SAFE),
    ("near-miss platform directory", ["src/Platform/LinuxExtra/A.cpp"], SAFE),
    # tests and benchmarks
    ("unit test", ["tests/Domain/test_CpuModel.cpp"], SAFE),
    ("test mock", ["tests/Mocks/MockProbes.h"], SAFE),
    ("tool test", ["tests/tools/test_bench_sh.py"], SAFE),
    ("workflow step reader", ["tests/tools/workflow_steps.py"], SAFE),
    ("tests CMake", ["tests/CMakeLists.txt"], SAFE),
    ("benchmark source", ["benchmarks/bench_ProcessModel.cpp"], SAFE),
    ("benchmark CMake", ["benchmarks/CMakeLists.txt"], SAFE),
    ("benchmark baseline", ["perf-data/linux-ci-baseline.json"], SAFE),
    # embedded and generated assets
    ("embedded fallback theme", ["assets/themes/arctic-fire.toml"], SAFE),
    ("font", ["assets/fonts/Inter-Regular.ttf"], SAFE),
    ("icon", ["assets/icons/tasksmack.png"], SAFE),
    # CMake, presets and toolchains
    ("root CMake", ["CMakeLists.txt"], SAFE),
    ("presets", ["CMakePresets.json"], SAFE),
    ("CMake module", ["cmake/RCCompiler.cmake"], SAFE),
    ("toolchain file", ["cmake/toolchains/clang.cmake"], SAFE),
    ("clang-tidy config", [".clang-tidy"], SAFE),
    ("clang-format config", [".clang-format"], SAFE),
    # dependency declarations
    ("FetchContent declarations", ["cmake/Dependencies.cmake"], SAFE),
    ("dependency patch", ["cmake/patches/imgui.patch"], SAFE),
    ("GLAD requirements", ["requirements-glad.txt"], SAFE),
    ("docs site requirements", ["requirements-docs-site.txt"], SAFE),
    ("pre-commit config", [".pre-commit-config.yaml"], SAFE),
    # build and tidy helpers
    ("clang-tidy script", ["tools/clang-tidy.sh"], SAFE),
    ("tidy file selection", ["tools/tidy-changed-files.py"], SAFE),
    ("tidy result cache", ["tools/tidy-cache.py"], SAFE),
    ("shared shell helpers", ["tools/common.sh"], SAFE),
    ("fuzz build", [".clusterfuzzlite/build.sh"], SAFE),
    # the workflows the C++ jobs run in, and the local actions they use
    ("ci.yml", [".github/workflows/ci.yml"], SAFE),
    ("reusable build workflow", [".github/workflows/reusable-build-test.yml"], SAFE),
    ("LLVM setup action", [".github/actions/setup-llvm/action.yml"], SAFE),
    ("tidy result cache action", [".github/actions/tidy-result-cache/action.yml"], SAFE),
    ("FetchContent cache action script", [".github/actions/fetchcontent-cache/hash.py"], SAFE),
    # other workflows are not reviewed yet: build inputs until listed
    ("CodeQL workflow", [".github/workflows/codeql.yml"], SAFE),
    ("sanitizer workflow", [".github/workflows/sanitizers.yml"], SAFE),
    ("backport workflow", [".github/workflows/backport.yml"], SAFE),
    ("CodeQL config", [".github/codeql/codeql-config.yml"], SAFE),
    # documentation (ci.yml's docs set, unchanged)
    ("root Markdown", ["README.md"], DOCS_ONLY),
    ("Markdown anywhere", ["src/Platform/README.md"], DOCS_ONLY),
    ("docs site file", ["docs/index.html"], DOCS_ONLY),
    ("docs image", ["docs/images/screenshots/dark/processes.webp"], DOCS_ONLY),
    ("issue template", [".github/ISSUE_TEMPLATE/bug_report.md"], DOCS_ONLY),
    ("issue template config", [".github/ISSUE_TEMPLATE/config.yml"], DOCS_ONLY),
    ("pull request template", [".github/pull_request_template.md"], DOCS_ONLY),
    ("pull request template directory", [".github/PULL_REQUEST_TEMPLATE/ci.md"], DOCS_ONLY),
    ("licence", ["LICENSE"], DOCS_ONLY),
    ("agent guidance", ["AGENTS.md", ".github/skills/code-review/SKILL.md"], DOCS_ONLY),
    ("upper-case extension is not Markdown", ["NOTES.MD"], SAFE),
    ("near-miss docs directory", ["docsite/index.html"], SAFE),
    ("near-miss licence", ["LICENSE.txt"], SAFE),
    # automation-only (each allowlisted file alone is generated below)
    ("Renovate workflow and config", [".github/workflows/renovate.yml", ".github/renovate.json5"], AUTOMATION_ONLY),
    ("labeler and its workflow", [".github/labeler.yml", ".github/workflows/pr-labeler.yml"], AUTOMATION_ONLY),
    ("automation and docs", [".github/workflows/renovate.yml", "README.md"], AUTOMATION_ONLY),
    ("automation and a template", [".github/dependabot.yml", ".github/ISSUE_TEMPLATE/feature_request.md"], AUTOMATION_ONLY),
    # mixed with build inputs
    ("Renovate workflow and a source", [".github/workflows/renovate.yml", "src/main.cpp"], SAFE),
    ("Dependabot config and ci.yml", [".github/dependabot.yml", ".github/workflows/ci.yml"], SAFE),
    ("Renovate config and dependency pins", [".github/renovate.json5", "cmake/Dependencies.cmake"], SAFE),
    ("labeler and a test", [".github/labeler.yml", "tests/App/test_Shell.cpp"], SAFE),
    ("automation and an unknown file", [".github/CODEOWNERS", "notes.txt"], SAFE),
    # unknown and near-miss paths
    ("unknown root file", ["notes.txt"], SAFE),
    ("new workflow", [".github/workflows/new-automation.yml"], SAFE),
    ("near-miss Renovate config", [".github/renovate.json"], SAFE),
    ("case differs", [".github/workflows/Renovate.yml"], SAFE),
    ("other extension", [".github/labeler.yaml"], SAFE),
    ("backup copy", [".github/workflows/renovate.yml.bak"], SAFE),
    ("same name elsewhere", ["sub/.github/CODEOWNERS"], SAFE),
    ("root CODEOWNERS (a different location)", ["CODEOWNERS"], SAFE),
    # the classifier itself: always every job
    ("classifier", ["tools/ci-change-classifier.py"], SAFE),
    ("classifier tests", ["tests/tools/test_ci_change_classifier.py"], SAFE),
    ("classifier and docs", ["tools/ci-change-classifier.py", "README.md"], SAFE),
    ("classifier and automation", ["tools/ci-change-classifier.py", ".github/workflows/renovate.yml"], SAFE),
    ("classifier tests and automation", ["tests/tools/test_ci_change_classifier.py", ".github/labeler.yml"], SAFE),
    ("ci.yml and automation", [".github/workflows/ci.yml", ".github/workflows/renovate.yml"], SAFE),
    ("empty change", [], SAFE),
]


def run_classifier(*args: str, cwd: Path | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    previous = Path.cwd()
    try:
        if cwd is not None:
            os.chdir(cwd)
        with redirect_stdout(out), redirect_stderr(err):
            code = classifier.main(list(args))
    finally:
        os.chdir(previous)
    return code, out.getvalue(), err.getvalue()


def parse(text: str) -> dict[str, bool]:
    values = {}
    for line in text.splitlines():
        name, _, value = line.partition("=")
        values[name] = {"true": True, "false": False}[value]
    return values


class ClassificationTable(unittest.TestCase):
    def test_cases(self) -> None:
        for description, paths, expected in CASES:
            with self.subTest(description, paths=paths):
                self.assertEqual(classifier.classify(paths), expected)

    def test_each_automation_file_alone(self) -> None:
        for path in classifier.AUTOMATION:
            with self.subTest(path):
                self.assertEqual(classifier.classify([path]), AUTOMATION_ONLY)

    def test_command_line_prints_every_output_in_order(self) -> None:
        code, out, err = run_classifier("--paths", ".github/workflows/renovate.yml")
        self.assertEqual(code, 0)
        self.assertEqual(out, "docs_only=false\nlinux_only=false\nwindows_only=false\nautomation_only=true\n")
        self.assertIn("automation", err)
        self.assertIn(".github/workflows/renovate.yml", err)

    def test_docs_only_and_automation_only_never_both(self) -> None:
        for _, paths, _ in CASES:
            values = classifier.classify(paths)
            self.assertFalse(values["docs_only"] and values["automation_only"], paths)

    def test_safe_values_run_everything(self) -> None:
        self.assertEqual(classifier.safe_values(), SAFE)


class Globs(unittest.TestCase):
    """The paths-filter globs ci.yml used before the classifier keep their meaning (#2130)."""

    def check(self, pattern: str, matches: list[str], misses: list[str]) -> None:
        regex = classifier._glob(pattern)
        for path in matches:
            self.assertTrue(regex.match(path), f"{pattern} should match {path}")
        for path in misses:
            self.assertFalse(regex.match(path), f"{pattern} should not match {path}")

    def test_markdown_anywhere(self) -> None:
        self.check("**/*.md", ["README.md", "a/b/c.md", ".github/x.md"], ["README.markdown", "a.MD", "a.md/x"])

    def test_directory_tree(self) -> None:
        self.check("docs/**", ["docs/a", "docs/a/b.png"], ["docs", "docsx/a", "a/docs/b"])

    def test_platform_sources(self) -> None:
        self.check(
            "src/Platform/Linux/**/*.cpp",
            ["src/Platform/Linux/A.cpp", "src/Platform/Linux/x/y/A.cpp"],
            ["src/Platform/Linux/A.h", "src/Platform/Linux/A.cpp.in", "src/Platform/LinuxX/A.cpp", "x/src/Platform/Linux/A.cpp"],
        )

    def test_single_segment_star(self) -> None:
        self.check(".github/workflows/reusable-*.yml", [".github/workflows/reusable-build-test.yml"],
                   [".github/workflows/reusable/x.yml", ".github/workflows/reusable-x.yaml"])


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, encoding="utf-8").stdout


def make_repo(root: Path) -> Path:
    repo = root / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "test@example.com")
    git(repo, "config", "user.name", "Test")
    git(repo, "config", "commit.gpgsign", "false")
    git(repo, "config", "core.autocrlf", "false")
    return repo


def write(repo: Path, path: str, text: str = "x\n") -> None:
    target = repo / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(text.encode("utf-8"))


def commit(repo: Path, message: str) -> None:
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "--allow-empty", "-m", message)


class GitDiffInput(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = make_repo(Path(self._tmp.name))
        for path in (".github/workflows/renovate.yml", ".github/labeler.yml", "src/Core/A.cpp", "README.md"):
            write(self.repo, path)
        commit(self.repo, "base")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def classify_change(self) -> dict[str, bool]:
        commit(self.repo, "change")
        code, out, _ = run_classifier("--base", "HEAD~1", "--head", "HEAD", cwd=self.repo)
        self.assertEqual(code, 0)
        return parse(out)

    def test_modified_automation_file(self) -> None:
        write(self.repo, ".github/workflows/renovate.yml", "changed\n")
        self.assertEqual(self.classify_change(), AUTOMATION_ONLY)

    def test_deleted_automation_file(self) -> None:
        (self.repo / ".github/labeler.yml").unlink()
        self.assertEqual(self.classify_change(), AUTOMATION_ONLY)

    def test_deleted_source_is_a_build_input(self) -> None:
        (self.repo / "src/Core/A.cpp").unlink()
        self.assertEqual(self.classify_change(), SAFE)

    def test_rename_out_of_the_build_counts_its_old_path(self) -> None:
        git(self.repo, "mv", "src/Core/A.cpp", "A.md")
        self.assertEqual(self.classify_change(), SAFE)

    def test_rename_into_the_build_counts_its_new_path(self) -> None:
        git(self.repo, "mv", "README.md", "src/Core/README.cpp")
        self.assertEqual(self.classify_change(), SAFE)

    def test_rename_between_automation_files(self) -> None:
        git(self.repo, "mv", ".github/labeler.yml", ".github/dependabot.yml")
        self.assertEqual(self.classify_change(), AUTOMATION_ONLY)

    def test_rename_of_an_allowlisted_file_to_an_unlisted_name(self) -> None:
        git(self.repo, "mv", ".github/labeler.yml", ".github/labeler.yaml")
        self.assertEqual(self.classify_change(), SAFE)

    def test_unusual_file_names(self) -> None:
        write(self.repo, "docs/a file with spaces & ünïcode.md")
        write(self.repo, ".github/workflows/renovate.yml", "changed\n")
        self.assertEqual(self.classify_change(), AUTOMATION_ONLY)

    def test_empty_change_is_full_validation(self) -> None:
        self.assertEqual(self.classify_change(), SAFE)

    def test_failed_git_diff_is_full_validation(self) -> None:
        code, out, err = run_classifier("--base", "no-such-commit", "--head", "HEAD", cwd=self.repo)
        self.assertEqual(code, 0)
        self.assertEqual(parse(out), SAFE)
        self.assertIn("git diff failed", err)

    def test_outside_a_repository_is_full_validation(self) -> None:
        outside = Path(self._tmp.name) / "not-a-repo"
        outside.mkdir()
        env_ceiling = os.environ.get("GIT_CEILING_DIRECTORIES")
        os.environ["GIT_CEILING_DIRECTORIES"] = str(Path(self._tmp.name))
        try:
            code, out, _ = run_classifier("--base", "HEAD~1", cwd=outside)
        finally:
            if env_ceiling is None:
                del os.environ["GIT_CEILING_DIRECTORIES"]
            else:
                os.environ["GIT_CEILING_DIRECTORIES"] = env_ceiling
        self.assertEqual(code, 0)
        self.assertEqual(parse(out), SAFE)


# Files the skipped C++ jobs read (directly, or through the scripts and actions they run). None may
# name an allowlisted file outside a comment.
def build_inputs() -> list[Path]:
    files = [REPO / "CMakeLists.txt", REPO / "CMakePresets.json", REPO / ".github/workflows/reusable-build-test.yml"]
    for pattern in ("src/**/CMakeLists.txt", "tests/**/CMakeLists.txt", "benchmarks/**/CMakeLists.txt",
                    "cmake/**/*.cmake", ".github/actions/**/*", "tools/*.sh", "tools/*.ps1", "tools/*.py"):
        files.extend(path for path in REPO.glob(pattern) if path.is_file())
    return sorted(set(files) - {SCRIPT})


def reference(path: str) -> re.Pattern[str]:
    """A mention of `path` or of its file name, not as part of a longer name (pr-labeler.yml is not labeler.yml)."""
    name = re.escape(path.rsplit("/", 1)[-1])
    return re.compile(r"(?<![\w.-])(?:" + re.escape(path) + "|" + name + r")(?![\w-])")


def automation_check_scripts() -> list[str]:
    step = find_step(CI, "Run the tool tests that read automation files", job="automation-checks")
    return re.findall(r"tests/tools/[\w-]+\.py", step.run)


# tests/tools scripts that name an allowlisted file but never read the repository's copy.
NAMES_WITHOUT_READING = {
    "test_tidy_changed_files.py": "writes its own pr-labeler.yml into a throwaway repository",
}


class AllowlistReview(unittest.TestCase):
    def test_every_entry_exists_and_has_a_reason(self) -> None:
        for path, reason in classifier.AUTOMATION.items():
            with self.subTest(path):
                self.assertTrue((REPO / path).is_file(), f"{path} is allowlisted but missing; update AUTOMATION")
                self.assertGreater(len(reason.split()), 3)

    def test_entries_are_exact_paths_in_no_other_set(self) -> None:
        for path in classifier.AUTOMATION:
            with self.subTest(path):
                self.assertNotRegex(path, r"[*?\[]")
                self.assertEqual(classifier.category(path), "automation")

    def test_no_build_input_names_an_automation_file(self) -> None:
        patterns = {path: reference(path) for path in classifier.AUTOMATION}
        for source in build_inputs():
            for number, line in enumerate(source.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                code = line.strip()
                if code.startswith(("#", "//", "<#")):
                    continue
                code = re.split(r"\s#\s", code, maxsplit=1)[0]
                for path, pattern in patterns.items():
                    with self.subTest(f"{source.relative_to(REPO).as_posix()}:{number}"):
                        self.assertIsNone(pattern.search(code), f"names {path}: {line.strip()}")

    def test_tool_tests_that_name_an_automation_file_run_in_automation_checks(self) -> None:
        scripts = automation_check_scripts()
        for script in scripts:
            self.assertTrue((REPO / script).is_file(), script)
        patterns = [reference(path) for path in classifier.AUTOMATION]
        for test in sorted((REPO / "tests" / "tools").glob("*.py")):
            text = test.read_text(encoding="utf-8")
            if not any(pattern.search(text) for pattern in patterns):
                continue
            with self.subTest(test.name):
                relative = test.relative_to(REPO).as_posix()
                self.assertTrue(
                    relative in scripts or test.name in NAMES_WITHOUT_READING,
                    f"{relative} names an automation file: run it in ci.yml's automation-checks",
                )

    def test_full_validation_files_are_in_no_skipping_set(self) -> None:
        for pattern in classifier.FULL_VALIDATION:
            with self.subTest(pattern):
                literal = pattern.replace("**", "x").replace("*", "x")
                self.assertFalse(classifier.is_docs(literal))
                self.assertFalse(classifier.is_automation(literal))
                self.assertEqual(classifier.classify([literal]), SAFE)


class WorkflowContract(unittest.TestCase):
    """ci.yml reads the classifier's outputs under the same names, with the same safe values."""

    def test_detect_job_outputs(self) -> None:
        text = CI.read_text(encoding="utf-8")
        for name in NAMES:
            with self.subTest(name):
                self.assertIn(f"      {name}: ${{{{ steps.set_output.outputs.{name} }}}}\n", text)

    def test_safe_outputs_line(self) -> None:
        step = find_step(CI, "Classify the changes", job="detect-docs-only")
        match = re.search(r"^SAFE_OUTPUTS='([^']*)'$", step.run, re.MULTILINE)
        self.assertIsNotNone(match)
        expected = " ".join(f"{output.name}={'true' if output.safe else 'false'}" for output in classifier.OUTPUTS)
        self.assertEqual(match.group(1), expected)

    def test_classifier_runs_from_the_base_branch(self) -> None:
        step = find_step(CI, "Classify the changes", job="detect-docs-only")
        self.assertIn("git show 'HEAD^1:tools/ci-change-classifier.py'", step.run)
        self.assertNotIn("${{", step.run)

    def test_automation_checks_runs_this_test(self) -> None:
        self.assertIn("tests/tools/test_ci_change_classifier.py", automation_check_scripts())


def posix(path: Path) -> str:
    return str(path).replace("\\", "/")


def run_bash(script: str, cwd: Path, env: dict[str, str], tmp: Path) -> subprocess.CompletedProcess:
    path = tmp / "step.sh"
    path.write_text(script + "\n", encoding="utf-8", newline="\n")
    return subprocess.run(
        # GitHub's `shell: bash` runs a step as `bash --noprofile --norc -eo pipefail {0}`.
        [BASH, "--noprofile", "--norc", "-eo", "pipefail", posix(path)],
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
        env=env,
        timeout=120,
    )


@unittest.skipUnless(BASH, "needs bash")
class DetectStep(unittest.TestCase):
    """ci.yml's "Classify the changes" step on throwaway merge commits, as actions/checkout leaves them."""

    STEP = find_step(CI, "Classify the changes", job="detect-docs-only")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.repo = make_repo(self.tmp)
        self.runner_temp = self.tmp / "runner-temp"
        self.runner_temp.mkdir()
        # The step runs python3; a shim makes that this interpreter on every OS.
        shims = self.tmp / "shims"
        shims.mkdir()
        shim = shims / "python3"
        shim.write_text(f'#!/bin/sh\nexec "{posix(Path(sys.executable))}" "$@"\n', encoding="utf-8", newline="\n")
        shim.chmod(0o755)
        self.path = posix(shims) if os.name != "nt" else str(shims)
        write(self.repo, "tools/ci-change-classifier.py", SCRIPT.read_text(encoding="utf-8"))
        for path in (".github/workflows/renovate.yml", "src/Core/A.cpp", "src/Platform/Linux/L.cpp", "README.md"):
            write(self.repo, path)
        commit(self.repo, "base")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def merge(self, *changes: str, base_change=None) -> None:
        """A pull request branch changing `changes`, merged with --no-ff the way refs/pull/N/merge is."""
        git(self.repo, "checkout", "-q", "-b", "pr")
        for path in changes:
            write(self.repo, path, "changed\n")
        commit(self.repo, "pr")
        git(self.repo, "checkout", "-q", "main")
        if base_change is not None:
            base_change()
            commit(self.repo, "base moved on")
        git(self.repo, "merge", "-q", "--no-ff", "--no-edit", "pr")

    def run_step(self, **overrides: str) -> tuple[subprocess.CompletedProcess, dict[str, str], str]:
        output = self.tmp / "output"
        summary = self.tmp / "summary"
        output.write_text("", encoding="utf-8")
        summary.write_text("", encoding="utf-8")
        env = {
            "EVENT_NAME": "pull_request",
            "HEAD_REPO": "fork/tasksmack",
            "REPOSITORY": "mgradwohl/tasksmack",
            "PR_AUTHOR": "contributor",
            "HEAD_REF": "feature",
            **overrides,
        }
        self.assertEqual(set(self.STEP.env), {"EVENT_NAME", "HEAD_REPO", "REPOSITORY", "PR_AUTHOR", "HEAD_REF"})
        full_env = {
            **os.environ,
            **env,
            "PATH": self.path + os.pathsep + os.environ.get("PATH", ""),
            "RUNNER_TEMP": posix(self.runner_temp),
            "GITHUB_OUTPUT": posix(output),
            "GITHUB_STEP_SUMMARY": posix(summary),
        }
        result = run_bash(self.STEP.run, self.repo, full_env, self.tmp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = output.read_text(encoding="utf-8").splitlines()
        self.assertEqual([line.partition("=")[0] for line in lines], NAMES, lines)
        return result, {k: v for k, v in (line.split("=", 1) for line in lines)}, summary.read_text(encoding="utf-8")

    @staticmethod
    def text(values: dict[str, bool]) -> dict[str, str]:
        return {name: "true" if value else "false" for name, value in values.items()}

    def test_renovate_pin_only(self) -> None:
        """#3049's change: one action pin in renovate.yml. The C++ jobs skip."""
        self.merge(".github/workflows/renovate.yml")
        result, outputs, summary = self.run_step()
        self.assertEqual(outputs, self.text(AUTOMATION_ONLY), result.stdout + result.stderr)
        self.assertIn("automation_only=true", summary)

    def test_source_change(self) -> None:
        self.merge("src/Core/A.cpp")
        self.assertEqual(self.run_step()[1], self.text(SAFE))

    def test_docs_change(self) -> None:
        self.merge("README.md", "docs/guide.md")
        self.assertEqual(self.run_step()[1], self.text(DOCS_ONLY))

    def test_linux_platform_change(self) -> None:
        self.merge("src/Platform/Linux/L.cpp")
        self.assertEqual(self.run_step()[1], self.text(LINUX_ONLY))

    def test_only_the_pull_request_side_counts(self) -> None:
        """A source change that reached the base branch after the branch point isn't the PR's."""
        self.merge(".github/workflows/renovate.yml", base_change=lambda: write(self.repo, "src/Core/A.cpp", "base\n"))
        self.assertEqual(self.run_step()[1], self.text(AUTOMATION_ONLY))

    def test_classifier_change_uses_the_base_copy(self) -> None:
        """A pull request that breaks the classifier is still judged by the base branch's copy."""
        git(self.repo, "checkout", "-q", "-b", "pr")
        write(self.repo, "tools/ci-change-classifier.py", "print('automation_only=true')\n")
        write(self.repo, ".github/workflows/renovate.yml", "changed\n")
        commit(self.repo, "pr")
        git(self.repo, "checkout", "-q", "main")
        git(self.repo, "merge", "-q", "--no-ff", "--no-edit", "pr")
        self.assertEqual(self.run_step()[1], self.text(SAFE))

    def test_no_classifier_on_the_base_branch(self) -> None:
        """This pull request's own case: the base branch predates the classifier."""
        git(self.repo, "rm", "-q", "tools/ci-change-classifier.py")
        commit(self.repo, "base without the classifier")
        self.merge(".github/workflows/renovate.yml")
        result, outputs, summary = self.run_step()
        self.assertEqual(outputs, self.text(SAFE))
        self.assertIn("no tools/ci-change-classifier.py", summary)

    def broken_base_classifier(self, body: str) -> subprocess.CompletedProcess:
        write(self.repo, "tools/ci-change-classifier.py", body)
        commit(self.repo, "broken classifier on the base branch")
        self.merge(".github/workflows/renovate.yml")
        result, outputs, _ = self.run_step()
        self.assertEqual(outputs, self.text(SAFE))
        return result

    def test_classifier_crash(self) -> None:
        result = self.broken_base_classifier("import sys\nsys.exit(3)\n")
        self.assertIn("::warning title=Change classification::tools/ci-change-classifier.py failed", result.stdout)

    def test_classifier_output_missing_a_line(self) -> None:
        result = self.broken_base_classifier(
            "print('docs_only=false')\nprint('linux_only=false')\nprint('windows_only=false')\n")
        self.assertIn("unexpected classifier output", result.stdout)

    def test_classifier_output_out_of_order(self) -> None:
        self.broken_base_classifier(
            "print('automation_only=true')\nprint('docs_only=false')\nprint('linux_only=false')\n"
            "print('windows_only=false')\n")

    def test_classifier_output_not_boolean(self) -> None:
        self.broken_base_classifier(
            "print('docs_only=false')\nprint('linux_only=false')\nprint('windows_only=false')\n"
            "print('automation_only=yes')\n")

    def test_classifier_output_with_an_extra_line(self) -> None:
        self.broken_base_classifier(
            "print('docs_only=false')\nprint('linux_only=false')\nprint('windows_only=false')\n"
            "print('automation_only=true')\nprint('docs_only=true')\n")

    def test_not_a_merge_commit(self) -> None:
        write(self.repo, ".github/workflows/renovate.yml", "changed\n")
        commit(self.repo, "a plain commit")
        result, outputs, _ = self.run_step()
        self.assertEqual(outputs, self.text(SAFE))
        self.assertIn("not the pull request's merge commit", result.stdout)

    def test_push_schedule_and_dispatch_get_every_job(self) -> None:
        self.merge(".github/workflows/renovate.yml")
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event):
                self.assertEqual(self.run_step(EVENT_NAME=event)[1], self.text(SAFE))

    def test_changelog_pull_request_gets_every_job(self) -> None:
        self.merge("CHANGELOG.md")
        outputs = self.run_step(HEAD_REPO="mgradwohl/tasksmack", PR_AUTHOR="github-actions[bot]",
                                HEAD_REF="chore/changelog-v0.9.1")[1]
        self.assertEqual(outputs, self.text(SAFE))

    def test_branch_name_is_data(self) -> None:
        self.merge(".github/workflows/renovate.yml")
        outputs = self.run_step(HEAD_REF='x"; echo docs_only=true >> "$GITHUB_OUTPUT"; #')[1]
        self.assertEqual(outputs, self.text(AUTOMATION_ONLY))


# A pull request whose every job ran and passed.
FULL_PR = {
    "DETECT_RESULT": "success",
    "DOCS_ONLY": "false",
    "LINUX_ONLY": "false",
    "WINDOWS_ONLY": "false",
    "AUTOMATION_ONLY": "false",
    "AUTOMATION_RESULT": "skipped",
    "EVENT_NAME": "pull_request",
    "SCHEDULE": "",
    "REF": "refs/pull/1/merge",
    "LINUX_RESULT": "success",
    "WINDOWS_RESULT": "success",
    "LINUX_RELEASE_RESULT": "success",
    "WINDOWS_RELEASE_RESULT": "skipped",
    "LINUX_UNITY_RESULT": "skipped",
    "DOCS_RESULT": "success",
    "LINUX_TIDY_MODE": "changed",
    "LINUX_TIDY_OUTCOME": "success",
    "RUN_URL": "https://example.invalid/run",
    "STATIC_ANALYSIS_WINDOWS_RESULT": "success",
    "ASAN_UBSAN_RESULT": "success",
}
SKIPPED_CPP = {
    "LINUX_RESULT": "skipped",
    "WINDOWS_RESULT": "skipped",
    "LINUX_RELEASE_RESULT": "skipped",
    "LINUX_TIDY_MODE": "",
    "LINUX_TIDY_OUTCOME": "",
    "STATIC_ANALYSIS_WINDOWS_RESULT": "skipped",
    "ASAN_UBSAN_RESULT": "skipped",
}
AUTOMATION_PR = {**FULL_PR, **SKIPPED_CPP, "AUTOMATION_ONLY": "true", "AUTOMATION_RESULT": "success"}
DOCS_PR = {**FULL_PR, **SKIPPED_CPP, "DOCS_ONLY": "true"}
PUSH_MAIN = {
    **FULL_PR,
    "EVENT_NAME": "push",
    "REF": "refs/heads/main",
    "LINUX_TIDY_MODE": "",
    "LINUX_TIDY_OUTCOME": "",
    "STATIC_ANALYSIS_WINDOWS_RESULT": "skipped",
    "ASAN_UBSAN_RESULT": "skipped",
}

# (what the case covers, job results, whether CI Success passes)
GATE_CASES = [
    ("full pull request", FULL_PR, True),
    ("docs-only pull request", DOCS_PR, True),
    ("automation-only pull request (#3049's change)", AUTOMATION_PR, True),
    ("push to main", PUSH_MAIN, True),
    ("automation-only, automation checks failed", {**AUTOMATION_PR, "AUTOMATION_RESULT": "failure"}, False),
    ("automation-only, automation checks skipped", {**AUTOMATION_PR, "AUTOMATION_RESULT": "skipped"}, False),
    ("automation-only, automation checks cancelled", {**AUTOMATION_PR, "AUTOMATION_RESULT": "cancelled"}, False),
    ("automation-only, a build ran and failed", {**AUTOMATION_PR, "LINUX_RESULT": "failure"}, False),
    ("automation-only, a build cancelled", {**AUTOMATION_PR, "WINDOWS_RESULT": "cancelled"}, False),
    ("automation-only, docs hygiene failed", {**AUTOMATION_PR, "DOCS_RESULT": "failure"}, False),
    ("C++ jobs skipped without a classification that allows it",
     {**FULL_PR, **SKIPPED_CPP}, False),
    ("automation checks failed on a full pull request", {**FULL_PR, "AUTOMATION_RESULT": "failure"}, False),
    ("automation-only for a push", {**PUSH_MAIN, **SKIPPED_CPP, "AUTOMATION_ONLY": "true",
                                    "AUTOMATION_RESULT": "skipped"}, False),
    ("both docs_only and automation_only", {**AUTOMATION_PR, "DOCS_ONLY": "true"}, False),
    ("change detection failed", {**AUTOMATION_PR, "DETECT_RESULT": "failure", "DOCS_ONLY": "",
                                 "LINUX_ONLY": "", "WINDOWS_ONLY": "", "AUTOMATION_ONLY": "",
                                 "AUTOMATION_RESULT": "skipped"}, False),
    ("change detection cancelled", {**FULL_PR, **SKIPPED_CPP, "DETECT_RESULT": "cancelled", "DOCS_ONLY": "",
                                    "LINUX_ONLY": "", "WINDOWS_ONLY": "", "AUTOMATION_ONLY": ""}, False),
    ("an output missing", {**AUTOMATION_PR, "AUTOMATION_ONLY": ""}, False),
    ("an output not boolean", {**FULL_PR, "LINUX_ONLY": "maybe"}, False),
]


@unittest.skipUnless(BASH, "needs bash")
class GateStep(unittest.TestCase):
    """CI Success's "Check all required jobs passed" step on job results."""

    STEP = find_step(CI, "Check all required jobs passed", job="ci-success")

    def test_env_is_covered(self) -> None:
        self.assertEqual(set(self.STEP.env), set(FULL_PR))

    def test_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            summary = tmp / "summary"
            for description, results, passes in GATE_CASES:
                with self.subTest(description):
                    summary.write_text("", encoding="utf-8")
                    env = {**os.environ, **results, "GITHUB_STEP_SUMMARY": posix(summary)}
                    result = run_bash(self.STEP.run, tmp, env, tmp)
                    self.assertEqual(result.returncode == 0, passes, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
