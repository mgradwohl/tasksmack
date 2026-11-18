#!/usr/bin/env python3
"""
tools/ci-change-classifier.py -- Decide which of ci.yml's pull-request jobs a change needs (#3104).

Usage:
    tools/ci-change-classifier.py --base <commit> [--head <commit>]
    tools/ci-change-classifier.py --paths <path>...

Prints one `<output>=true|false` line per output below, in this order, for ci.yml's detect-docs-only
job to hand its callers. Each changed path's category goes to stderr, for the job log. Standard
library only, like the tools/check-*.py checkers. With --base, the changes are `git diff --no-renames
<base> <head>`: a deleted path counts, and a rename counts as its old and its new path.

Outputs, the contract every caller reads (ci.yml's build jobs; #3103 adds docs_site for docs-hygiene):

  docs_only        Every changed path is documentation (DOCS). The C++ builds, both clang-tidy runs and
                   ASan+UBSan skip.
  linux_only       Every changed path is a Linux platform .cpp (LINUX_PLATFORM). The Windows debug build
                   and its clang-tidy skip.
  windows_only     Every changed path is a Windows platform .cpp (WINDOWS_PLATFORM). The Linux builds
                   and the Linux clang-tidy skip.
  automation_only  Every changed path is documentation or a reviewed automation file (AUTOMATION), at
                   least one is automation, and none is in FULL_VALIDATION. The C++ jobs skip as for
                   docs_only, and ci.yml's automation-checks job runs the tool tests that read those
                   files instead. Never true together with docs_only.
  docs_site        Some changed path is an input of the docs site's browser smoke test (DOCS_SITE: the
                   site itself, the test, its lockfile). docs-hygiene runs the Playwright test only when
                   it's true (#3103); its link audit runs every time.
  dev_tools_only   Every changed path is documentation, an automation file or a local developer tool
                   (DEV_TOOLS: the perf, profiling and idle-measurement scripts and their tests, #4248), at
                   least one is a developer tool, and none is in FULL_VALIDATION. The C++ jobs skip as for
                   docs_only, and ci.yml's dev-tools-checks jobs run the tool tests that read those files
                   (DEV_TOOL_TESTS) instead, with the automation tests. Never true together with
                   docs_only or automation_only.

Each output has a safe value: the one that runs every job its flag can skip (false for each *_only
flag, true for docs_site). Every output takes its safe value when the change is empty, when git fails, or when any
changed path is in FULL_VALIDATION or matches no set. A path no set names is a build input until
someone shows otherwise. The first three outputs keep the paths-filter globs ci.yml used before
this script (#2130); codeql.yml's docs-only skip still uses the same DOCS set.

Why each AUTOMATION and DEV_TOOLS file can't change what the skipped jobs check. Those jobs build and test the
C++ tree and run ctest: they read src/, tests/, benchmarks/, assets/, cmake/, the CMake files,
presets and dependency declarations, the build and tidy scripts under tools/, ci.yml, the reusable
workflow and the local actions under .github/actions, none of which is in AUTOMATION. The only part
of them that reads an AUTOMATION or DEV_TOOLS file is ctest's tool tests (tests/tools, tools/test-*.ps1);
automation-checks and the dev-tools-checks jobs run those, and test_ci_change_classifier.py fails when a
tool test names such a file without the matching job running it. The reasons, file by file, are in AUTOMATION below. To add a
file, show the same for it (nothing in the skipped jobs reads it, or automation-checks runs what
does) and add a case to tests/tools/test_ci_change_classifier.py.

The workflow-level checks are outside this script and still run on every pull request: pre-commit
(actionlint, check-yaml, check-cache-prune-triggers, REUSE), CodeQL's Actions analysis, Dependency
Review and OSV-Scanner.

Adding an output (#3103's docs_site): append an Output to OUTPUTS with its safe value and its rule,
add it to the detect job's outputs and its SAFE_OUTPUTS line in ci.yml, and add cases to the tests.
Changing this script, its tests, ci.yml, the reusable workflow or a local action always gives the
safe values (FULL_VALIDATION), and ci.yml runs the base branch's copy of this script, so a change
here can't skip the jobs that would test it.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass


def _glob(pattern: str) -> re.Pattern[str]:
    """A paths-filter (picomatch) glob as a regex: `**/` is any number of directories (none too), a
    trailing `/**` everything below, `*` any run of characters in one path segment. Case-sensitive,
    and dot files match, as paths-filter's `dot: true` does."""
    regex = ""
    index = 0
    while index < len(pattern):
        if pattern.startswith("**/", index):
            regex += "(?:[^/]*/)*"
            index += 3
        elif pattern.startswith("/**", index) and index + 3 == len(pattern):
            regex += "/.+"
            index += 3
        elif pattern[index] == "*":
            regex += "[^/]*"
            index += 1
        else:
            regex += re.escape(pattern[index])
            index += 1
    return re.compile(regex + r"\Z")


def _matcher(patterns: Sequence[str]) -> Callable[[str], bool]:
    compiled = [_glob(pattern) for pattern in patterns]
    return lambda path: any(regex.match(path) for regex in compiled)


# Documentation: ci.yml's docs set since #1786, also codeql.yml's. Not an input to any C++ job.
DOCS = (
    "**/*.md",
    "docs/**",
    ".github/ISSUE_TEMPLATE/**",
    ".github/pull_request_template.md",
    ".github/PULL_REQUEST_TEMPLATE/**",
    "LICENSE",
)

# Only a platform's .cpp files are its own. Its headers can be compiled into the other OS's build:
# the shared test binary includes about 20 header-only Linux parsers (test_LinuxOsInfo.cpp and others
# in COMMON_TEST_SOURCES), and Linux builds the Windows *Math.h headers (WINDOWS_MATH_TEST_SOURCES).
# So a header change runs both OSes' builds, or a break in the other OS's compile would only show on
# main (#2130).
LINUX_PLATFORM = ("src/Platform/Linux/**/*.cpp",)

# The docs site's browser smoke test's inputs (#3103): the Docsify site under docs/, the test itself and
# its hash-locked dependencies. Anything else can't change what tools/docs-site-smoke.py sees.
DOCS_SITE = (
    "docs/**",
    "tools/docs-site-smoke.py",
    "requirements-docs-site.in",
    "requirements-docs-site.txt",
)
WINDOWS_PLATFORM = ("src/Platform/Windows/**/*.cpp",)

# Automation files outside every C++ job's inputs, each with the reason. Exact paths, not globs: a
# new file next to one of these is a build input until it is reviewed and listed here.
AUTOMATION = {
    ".github/workflows/renovate.yml": (
        "the self-hosted Renovate run (schedule and dispatch only). No workflow calls it and no build "
        "reads it; tests/tools/test_workflow_run_steps.py runs its dry-run step, in automation-checks."
    ),
    ".github/renovate.json5": (
        "Renovate's configuration, read only by renovate.yml's Renovate run. The dependency pins it "
        "updates are in other files, which stay build inputs."
    ),
    ".github/dependabot.yml": (
        "Dependabot's configuration, read only by GitHub's Dependabot service. The action and "
        "requirements pins it updates are in other files, which stay build inputs."
    ),
    ".github/labeler.yml": "pr-labeler.yml's label globs, read only by actions/labeler.",
    ".github/workflows/pr-labeler.yml": (
        "labels pull requests (pull_request_target, so a pull request runs the base branch's copy). "
        "No workflow calls it and no build reads it."
    ),
    ".github/CODEOWNERS": "review routing, read only by GitHub.",
    ".github/actionlint.yaml": "actionlint's configuration, read only by the pre-commit actionlint hook.",
}

# Local developer tools (#4248): the performance, profiling and idle-measurement scripts a developer runs by
# hand. No workflow runs them, no build or C++ test reads them, and no build or CI script calls them; only
# their own tool tests do, and the dev-tools-checks jobs run those (DEV_TOOL_TESTS). Exact paths, as for
# AUTOMATION. Never here: anything a workflow, the build, a pre-commit or CI checker, coverage, release,
# signing or packaging reads, and the scripts Heavy Checks runs (bench.sh, bench-manifest.py,
# bench-across-launches.py, check-benchmark-regression.py, merge-benchmark-baselines.py, wayland-matrix.sh,
# ninja-build-profile.py), and the helpers the build and CI scripts share with these (common.sh, common.ps1).
_LOCAL = "a local developer script; no workflow, build or C++ test runs it, only its own tool test"
DEV_TOOLS = {
    # Linux
    "tools/profile-perf.sh": f"{_LOCAL} (test_profile_perf_sh.py).",
    "tools/profile-heap.sh": f"{_LOCAL} (test_profile_heap_sh.py).",
    "tools/analyze-perf.sh": f"{_LOCAL} (test_analyze_perf_sh.py).",
    "tools/time-trace.sh": f"{_LOCAL}; CMakeLists.txt names it only in a message().",
    "tools/pgo.sh": f"{_LOCAL} (test_pgo_sh.py); CMakeLists.txt names it only in a comment.",
    "tools/measure-idle.sh": f"{_LOCAL}; its readers are idle-sample.py and resize-perf-summary.awk.",
    "tools/idle-sample.py": f"{_LOCAL} (test_idle_sample.py), measure-idle.sh's sampler.",
    "tools/resize-perf-summary.awk": f"{_LOCAL} (test_resize_perf_summary_awk.py), measure-idle.sh's reader.",
    "tools/ui-compare.sh": f"{_LOCAL}; it builds two refs in scratch worktrees, never in CI.",
    "tools/ui-compare-report.py": f"{_LOCAL} (test_ui_compare_report.py), ui-compare.sh's report.",
    # Windows
    "tools/bench.ps1": f"{_LOCAL} (test-bench.ps1, test-bench-across-launches.ps1); CI benchmarks run bench.sh.",
    "tools/profile-etw.ps1": f"{_LOCAL} (test-profile-etw.ps1).",
    "tools/profile-etw-common.ps1": f"{_LOCAL}, shared by the ETW capture scripts (test-profile-etw*.ps1).",
    "tools/profile-etw-resize.ps1": f"{_LOCAL} (test-profile-etw-resize.ps1).",
    "tools/analyze-etw.ps1": f"{_LOCAL} (test-analyze-etw.ps1).",
    "tools/analyze-etw-common.ps1": f"{_LOCAL}, shared by the ETW analysis (test-analyze-etw.ps1).",
    "tools/TaskSmackCPU.wprp": "the ETW CPU recording profile, read only by profile-etw.ps1.",
    "tools/TaskSmackResize.wprp": "the ETW resize recording profile, read only by profile-etw-resize.ps1.",
    "tools/pgo.ps1": f"{_LOCAL}; CMakeLists.txt names it only in a comment.",
    "tools/measure-idle.ps1": f"{_LOCAL} (test-measure-idle.ps1).",
    "tools/idle-sample.ps1": f"{_LOCAL} (test-measure-idle.ps1), measure-idle.ps1's sampler.",
    "tools/resize-perf-summary.ps1": f"{_LOCAL} (test-resize-perf-summary.ps1), measure-idle.ps1's reader.",
    "tools/cmake-cache.ps1": (
        "the CMakeCache.txt reader only bench.ps1 and profile-etw-common.ps1 dot-source; no build or CI script "
        "reads it (bench-manifest.py names it in a comment), only its test (test-cmake-cache.ps1)."
    ),
    "perf-data/win-baseline.json": "bench.ps1's local baseline; no workflow reads it (CI's is linux-ci-baseline.json).",
    "perf-data/win-baseline.manifest.json": "win-baseline.json's manifest, read only by a local comparison.",
    # Their tests: what each runs is in DEV_TOOL_TESTS.
    "tests/tools/test_profile_perf_sh.py": "profile-perf.sh's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_pgo_sh.py": "pgo.sh's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_profile_heap_sh.py": "profile-heap.sh's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_analyze_perf_sh.py": "analyze-perf.sh's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_idle_sample.py": "idle-sample.py's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_resize_perf_summary_awk.py": "resize-perf-summary.awk's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_ui_compare_report.py": "ui-compare-report.py's tool test, run by ctest and by dev-tools-checks.",
    "tests/tools/test_ui_compare_sh.py": "ui-compare.sh's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-bench.ps1": "bench.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-bench-across-launches.ps1": "bench.ps1 -Baseline's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-cmake-cache.ps1": "cmake-cache.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-profile-etw.ps1": "profile-etw.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-profile-etw-resize.ps1": "profile-etw-resize.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-analyze-etw.ps1": "analyze-etw.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-measure-idle.ps1": "measure-idle.ps1's and idle-sample.ps1's tool test, run by ctest and by dev-tools-checks.",
    "tools/test-resize-perf-summary.ps1": "resize-perf-summary.ps1's tool test, run by ctest and by dev-tools-checks.",
}

# The tool tests the dev-tools-checks jobs run, by runner, in ctest's registrations' way (tests/CMakeLists.txt):
# the Python ones with TASKSMACK_TEST_BASH, the pwsh ones one at a time with TASKSMACK_TEST_PYTHON. Every
# tool test that names a DEV_TOOLS file is here (test_ci_change_classifier.py checks), with the automation
# tests, which a change that is both still needs.
DEV_TOOL_TESTS = {
    "linux": (
        "tests/tools/test_ci_change_classifier.py",
        "tests/tools/test_workflow_run_steps.py",
        "tests/tools/test_check_cache_prune_triggers.py",
        "tests/tools/test_profile_perf_sh.py",
        "tests/tools/test_pgo_sh.py",
        "tests/tools/test_profile_heap_sh.py",
        "tests/tools/test_analyze_perf_sh.py",
        "tests/tools/test_idle_sample.py",
        "tests/tools/test_resize_perf_summary_awk.py",
        "tests/tools/test_ui_compare_report.py",
        "tests/tools/test_ui_compare_sh.py",
    ),
    "windows": (
        "tools/test-bench.ps1",
        "tools/test-bench-across-launches.ps1",
        "tools/test-profile-etw.ps1",
        "tools/test-profile-etw-resize.ps1",
        "tools/test-analyze-etw.ps1",
        "tools/test-measure-idle.ps1",
        "tools/test-resize-perf-summary.ps1",
        "tools/test-cmake-cache.ps1",
    ),
}

# Always the safe values, whatever else the change holds: this classifier and its tests, the
# workflow that calls it, the reusable build workflow and the local actions the C++ jobs use. None
# is in DOCS or AUTOMATION; this list keeps it so if either is ever widened with a glob.
FULL_VALIDATION = (
    "tools/ci-change-classifier.py",
    "tools/run-dev-tool-tests.py",
    "tests/tools/test_ci_change_classifier.py",
    "tests/tools/workflow_steps.py",
    ".github/workflows/ci.yml",
    ".github/workflows/reusable-*.yml",
    ".github/actions/**",
)

is_docs = _matcher(DOCS)
is_linux_platform = _matcher(LINUX_PLATFORM)
is_windows_platform = _matcher(WINDOWS_PLATFORM)
is_docs_site = _matcher(DOCS_SITE)
is_full_validation = _matcher(FULL_VALIDATION)


def is_automation(path: str) -> bool:
    return path in AUTOMATION


def is_dev_tools(path: str) -> bool:
    return path in DEV_TOOLS


def _automation_only(paths: Sequence[str]) -> bool:
    if all(is_docs(path) for path in paths):
        return False  # docs_only's case
    return all(is_docs(path) or is_automation(path) for path in paths)


def _dev_tools_only(paths: Sequence[str]) -> bool:
    if not any(is_dev_tools(path) for path in paths):
        return False  # docs_only's or automation_only's case, or neither
    return all(is_docs(path) or is_automation(path) or is_dev_tools(path) for path in paths)


@dataclass(frozen=True)
class Output:
    name: str
    safe: bool  # the value that runs every job this output can skip
    rule: Callable[[Sequence[str]], bool]  # its value for a non-empty change with no FULL_VALIDATION path


OUTPUTS = (
    Output("docs_only", False, lambda paths: all(is_docs(path) for path in paths)),
    Output("linux_only", False, lambda paths: all(is_linux_platform(path) for path in paths)),
    Output("windows_only", False, lambda paths: all(is_windows_platform(path) for path in paths)),
    Output("automation_only", False, _automation_only),
    Output("docs_site", True, lambda paths: any(is_docs_site(path) for path in paths)),
    Output("dev_tools_only", False, _dev_tools_only),
)


def safe_values() -> dict[str, bool]:
    return {output.name: output.safe for output in OUTPUTS}


def classify(paths: Sequence[str]) -> dict[str, bool]:
    """Each output's value for a change to `paths` (repo-relative, forward slashes)."""
    if not paths or any(is_full_validation(path) for path in paths):
        return safe_values()
    return {output.name: output.rule(paths) for output in OUTPUTS}


def category(path: str) -> str:
    if is_full_validation(path):
        return "full-validation"
    if is_docs(path):
        return "docs"
    if is_automation(path):
        return "automation"
    if is_dev_tools(path):
        return "dev-tools"
    if is_linux_platform(path):
        return "linux-platform"
    if is_windows_platform(path):
        return "windows-platform"
    return "build-input"


def changed_paths(base: str, head: str) -> list[str]:
    """Every path `git diff --no-renames base head` names, sorted; raises on a git failure."""
    out = subprocess.run(
        ["git", "diff", "--name-status", "-z", "--no-renames", "--no-ext-diff", base, head, "--"],
        check=True,
        capture_output=True,
    ).stdout.decode("utf-8", errors="surrogateescape")
    fields = out.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    paths: set[str] = set()
    index = 0
    while index < len(fields):
        status = fields[index]
        # --no-renames means one path per entry; a copy or rename entry (two paths) is still read whole.
        count = 2 if status[:1] in ("R", "C") else 1
        names = fields[index + 1 : index + 1 + count]
        if not status or len(names) != count:
            raise ValueError(f"unexpected git diff --name-status output near {status!r}")
        paths.update(names)
        index += 1 + count
    return sorted(paths)


def render(values: dict[str, bool]) -> str:
    return "".join(f"{name}={'true' if value else 'false'}\n" for name, value in values.items())


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--base", help="commit to diff against (the pull request's base)")
    source.add_argument("--paths", nargs="+", metavar="PATH", help="classify these repo-relative paths")
    parser.add_argument("--head", default="HEAD", help="commit with the change (default: HEAD)")
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(newline="\n")  # name=value lines, never CRLF, wherever it runs

    if args.paths is not None:
        paths = sorted(set(args.paths))
    else:
        try:
            paths = changed_paths(args.base, args.head)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            detail = getattr(error, "stderr", None) or b""
            if isinstance(detail, bytes):
                detail = detail.decode("utf-8", errors="replace")
            print(f"ci-change-classifier: git diff failed ({error}) {detail.strip()}", file=sys.stderr)
            print("ci-change-classifier: full validation", file=sys.stderr)
            sys.stdout.write(render(safe_values()))
            return 0

    if not paths:
        print("ci-change-classifier: no changed paths; full validation", file=sys.stderr)
    for path in paths:
        print(f"  {category(path):<16} {path}", file=sys.stderr)
    sys.stdout.write(render(classify(paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
