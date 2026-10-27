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

Each output has a safe value: the one that runs every job its flag can skip (false for each *_only
flag, true for docs_site). Every output takes its safe value when the change is empty, when git fails, or when any
changed path is in FULL_VALIDATION or matches no set. A path no set names is a build input until
someone shows otherwise. The first three outputs keep the paths-filter globs ci.yml used before
this script (#2130); codeql.yml's docs-only skip still uses the same DOCS set.

Why each AUTOMATION file can't change what the skipped jobs check. Those jobs build and test the
C++ tree and run ctest: they read src/, tests/, benchmarks/, assets/, cmake/, the CMake files,
presets and dependency declarations, the build and tidy scripts under tools/, ci.yml, the reusable
workflow and the local actions under .github/actions, none of which is in AUTOMATION. The only part
of them that reads an AUTOMATION file is ctest's tool tests (tests/tools); automation-checks runs
those, and test_ci_change_classifier.py fails when a tests/tools script names an AUTOMATION file
without automation-checks running it. The reasons, file by file, are in AUTOMATION below. To add a
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

# Always the safe values, whatever else the change holds: this classifier and its tests, the
# workflow that calls it, the reusable build workflow and the local actions the C++ jobs use. None
# is in DOCS or AUTOMATION; this list keeps it so if either is ever widened with a glob.
FULL_VALIDATION = (
    "tools/ci-change-classifier.py",
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


def _automation_only(paths: Sequence[str]) -> bool:
    if all(is_docs(path) for path in paths):
        return False  # docs_only's case
    return all(is_docs(path) or is_automation(path) for path in paths)


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
