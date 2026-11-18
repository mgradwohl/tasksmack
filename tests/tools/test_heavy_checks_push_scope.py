#!/usr/bin/env python3
"""Tests for Heavy Checks' push scope (#4195): a push that changes code the benchmarks measure must reach the
benchmark-regression job. The list is derived, not kept by hand: every header that benchmarks/*.cpp includes
from the repository must match both heavy-checks.yml's `on: push: paths` filter and the push-scope step's case
list, and so must the build settings (presets, compiler options, the shared shell helpers)."""

import fnmatch
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "heavy-checks.yml"

# Roots a quoted include in benchmarks/*.cpp can resolve against (the benchmark target's include directories).
INCLUDE_ROOTS = ("src", "tests", "benchmarks")

# What the benchmarks' build and run read besides the sources they include.
BUILD_SETTINGS = ("CMakePresets.json", "cmake/CompilerOptions.cmake", "tools/common.sh")


def benchmarked_includes() -> dict[str, set[str]]:
    """Each repository file a benchmark includes with quotes, mapped to the benchmark sources that include it."""
    found: dict[str, set[str]] = {}
    for source in sorted((REPO / "benchmarks").glob("*.cpp")):
        for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', source.read_text(encoding="utf-8"), re.MULTILINE):
            for root in INCLUDE_ROOTS:
                candidate = REPO / root / name
                if candidate.is_file():
                    found.setdefault(candidate.relative_to(REPO).as_posix(), set()).add(source.name)
                    break
    return found


def push_filter() -> list[str]:
    text = WORKFLOW.read_text(encoding="utf-8")
    block = text.split("\n  push:\n", 1)[1].split("\n  schedule:", 1)[0]
    return re.findall(r"^      - '([^']+)'$", block, re.MULTILINE)


def scope_patterns() -> list[str]:
    text = WORKFLOW.read_text(encoding="utf-8")
    body = text.split("      benchmarks: ${{ steps.scope.outputs.benchmarks }}", 1)[1]
    listed = re.search(r'case "\$path" in\n(.*?)\)\n', body, re.DOTALL).group(1)
    return [p.strip() for p in listed.replace("\\\n", "").split("|") if p.strip()]


def matches(path: str, patterns: list[str]) -> bool:
    # The workflow filter's `**` and the case list's `*` both cross directories; fnmatch's `*` does too.
    return any(fnmatch.fnmatchcase(path, pattern.replace("**", "*")) for pattern in patterns)


class HeavyChecksPushScopeCoversBenchmarks(unittest.TestCase):
    def test_the_derivation_finds_the_known_benchmarked_headers(self) -> None:
        found = benchmarked_includes()
        for path in ("src/App/Panels/ProcessFilterCache.h", "src/App/ProcessColumnConfig.h", "tests/Mocks/MockProbes.h",
                     "src/Domain/ProcessModel.h"):
            self.assertIn(path, found, "the include scan stopped finding a header the benchmarks include")

    def test_every_benchmarked_include_is_in_the_push_filter(self) -> None:
        paths = push_filter()
        for path, users in sorted(benchmarked_includes().items()):
            with self.subTest(path):
                self.assertTrue(matches(path, paths), f"{path} (included by {sorted(users)}) is not in on.push.paths")

    def test_every_benchmarked_include_runs_the_benchmarks_on_a_push(self) -> None:
        patterns = scope_patterns()
        for path, users in sorted(benchmarked_includes().items()):
            with self.subTest(path):
                self.assertTrue(matches(path, patterns), f"{path} (included by {sorted(users)}) is not in push-scope")

    def test_the_build_settings_are_in_both(self) -> None:
        paths, patterns = push_filter(), scope_patterns()
        for path in BUILD_SETTINGS:
            with self.subTest(path):
                self.assertTrue((REPO / path).is_file())
                self.assertTrue(matches(path, paths), f"{path} is not in on.push.paths")
                self.assertTrue(matches(path, patterns), f"{path} is not in push-scope")


if __name__ == "__main__":
    unittest.main()
