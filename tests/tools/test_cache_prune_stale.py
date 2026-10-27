#!/usr/bin/env python3
"""Tests for tools/cache-prune-stale.py (#1583, #2528, #3101): cache-prune.yml keeps the newest entry of each
cache family and deletes the rest, so a family must keep apart entries that can't stand in for each other, such
as the x64 and ARM64 Windows configure-check caches."""

import importlib.util
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "cache-prune-stale.py"
spec = importlib.util.spec_from_file_location("cache_prune_stale", SCRIPT)
pruner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pruner)


def row(cache_id: int, key: str, created: str, size: int = 1000) -> str:
    return f"{cache_id}\t{key}\t{created}\t{size}"


def stale_keys(rows: list[str]) -> list[str]:
    return [entry[4] for entry in pruner.stale_entries(rows)]


class FamilyTests(unittest.TestCase):
    def test_ccache_strips_the_timestamp(self):
        self.assertEqual(
            pruner.family("ccache-Windows-ARM64-debug-llvm22.1.7-2026-10-18T01:02:03.456Z"),
            "ccache-Windows-ARM64-debug-llvm22.1.7",
        )

    def test_fetchcontent_strips_the_hash(self):
        self.assertEqual(pruner.family("Linux-fetchcontent-" + "a" * 64), "Linux-fetchcontent")

    def test_cmake_checks_keeps_the_architecture(self):
        self.assertEqual(
            pruner.family("Windows-cmake-checks-win-debug-11d91f5ce8bd586e-v1"), "Windows-cmake-checks-win-debug"
        )
        self.assertEqual(
            pruner.family("Windows-ARM64-cmake-checks-win-debug-68affe41d46a7699-v1"),
            "Windows-ARM64-cmake-checks-win-debug",
        )

    def test_tidy_results_strip_the_run(self):
        self.assertEqual(pruner.family("Linux-tidy-results-39169793986-1"), "Linux-tidy-results")

    def test_codeql_trap_strips_the_commit(self):
        # #3101: one ~640 MB entry per analyzed main commit; PRs fall back to the newest by prefix.
        self.assertEqual(
            pruner.family("codeql-trap-1-2.27.3-cpp-cbe76138db3f52ac081a951cc5d4e06ae5644d77"),
            "codeql-trap-1-2.27.3-cpp",
        )
        self.assertEqual(
            pruner.family("codeql-trap-1-2.27.3-actions-89fe5c7f9a1fea5978ca5b951a8d942ffb1ef708"),
            "codeql-trap-1-2.27.3-actions",
        )

    def test_single_key_caches_are_not_managed(self):
        for key in (
            "windows-llvm-22.1.7-ARM64-v2",
            "iwyu-llvm22-abc123-Linux-v1",
            "setup-python-Linux-x64-24.04-Ubuntu-python-3.12.3-pip-0123",
            "ccache-not-timestamped",
            "Windows-cmake-checks-win-debug",
            "codeql-trap-1-2.27.3-cpp-not-a-sha",
        ):
            with self.subTest(key=key):
                self.assertIsNone(pruner.family(key))


class StaleEntryTests(unittest.TestCase):
    def test_each_architecture_keeps_its_newest_configure_check_entry(self):
        # #2528: x64 and ARM64 save in turn; each must keep its own newest entry.
        rows = [
            row(1, "Windows-cmake-checks-win-debug-0000000000000001-v1", "2026-10-16T10:00:00Z"),
            row(2, "Windows-ARM64-cmake-checks-win-debug-0000000000000002-v1", "2026-10-16T11:00:00Z"),
            row(3, "Windows-cmake-checks-win-debug-11d91f5ce8bd586e-v1", "2026-10-17T10:00:00Z"),
            row(4, "Windows-ARM64-cmake-checks-win-debug-68affe41d46a7699-v1", "2026-10-18T10:00:00Z"),
            row(5, "Windows-cmake-checks-win-release-0000000000000005-v1", "2026-10-15T10:00:00Z"),
        ]
        self.assertEqual(
            sorted(stale_keys(rows)),
            [
                "Windows-ARM64-cmake-checks-win-debug-0000000000000002-v1",
                "Windows-cmake-checks-win-debug-0000000000000001-v1",
            ],
        )

    def test_each_architecture_keeps_its_newest_ccache_entry(self):
        rows = [
            row(10, "ccache-Linux-debug-llvm22-2026-10-17T01:00:00.000Z", "2026-10-17T01:00:00Z"),
            row(11, "ccache-Linux-ARM64-debug-llvm22-2026-10-17T02:00:00.000Z", "2026-10-17T02:00:00Z"),
            row(12, "ccache-Linux-debug-llvm22-2026-10-18T01:00:00.000Z", "2026-10-18T01:00:00Z"),
            row(13, "ccache-Linux-ARM64-debug-llvm22-2026-10-18T02:00:00.000Z", "2026-10-18T02:00:00Z"),
            row(14, "ccache-Windows-ARM64-debug-llvm22.1.7-2026-10-18T03:00:00.000Z", "2026-10-18T03:00:00Z"),
        ]
        self.assertEqual(
            sorted(stale_keys(rows)),
            [
                "ccache-Linux-ARM64-debug-llvm22-2026-10-17T02:00:00.000Z",
                "ccache-Linux-debug-llvm22-2026-10-17T01:00:00.000Z",
            ],
        )

    def test_other_families_and_single_key_caches(self):
        rows = [
            row(20, "Linux-fetchcontent-" + "1" * 64, "2026-10-10T00:00:00Z"),
            row(21, "Linux-fetchcontent-" + "2" * 64, "2026-10-12T00:00:00Z"),
            row(22, "Windows-fetchcontent-" + "3" * 64, "2026-10-11T00:00:00Z"),
            row(23, "Linux-tidy-results-100-1", "2026-10-11T00:00:00Z"),
            row(24, "Linux-tidy-results-101-2", "2026-10-12T00:00:00Z"),
            row(25, "windows-llvm-22.1.7-X64-v2", "2026-01-01T00:00:00Z"),
            row(26, "windows-llvm-22.1.7-ARM64-v2", "2026-01-02T00:00:00Z"),
        ]
        self.assertEqual(sorted(stale_keys(rows)), ["Linux-fetchcontent-" + "1" * 64, "Linux-tidy-results-100-1"])

    def test_codeql_trap_keeps_the_newest_per_language_and_cli_version(self):
        sha = "{:040x}".format
        rows = [
            row(50, f"codeql-trap-1-2.27.3-cpp-{sha(1)}", "2026-10-26T11:06:48Z", 642125253),
            row(51, f"codeql-trap-1-2.27.3-cpp-{sha(2)}", "2026-10-26T16:47:51Z", 642337562),
            row(52, f"codeql-trap-1-2.27.3-cpp-{sha(3)}", "2026-10-27T01:57:25Z", 644145509),
            row(53, f"codeql-trap-1-2.27.3-actions-{sha(4)}", "2026-10-26T10:00:00Z", 1000),
            row(54, f"codeql-trap-1-2.28.0-cpp-{sha(5)}", "2026-10-25T10:00:00Z", 1000),
            # The tidy result caches this pressure evicted are a family of their own.
            row(55, "Linux-tidy-results-40277744615-1", "2026-10-26T06:16:24Z", 13852),
            row(56, "Windows-tidy-results-40277744615-1", "2026-10-26T06:15:05Z", 12090),
        ]
        self.assertEqual(
            sorted(stale_keys(rows)),
            [f"codeql-trap-1-2.27.3-cpp-{sha(1)}", f"codeql-trap-1-2.27.3-cpp-{sha(2)}"],
        )

    def test_output_fields_and_order(self):
        rows = [
            row(30, "Linux-tidy-results-1-1", "2026-10-10T00:00:00Z", 5),
            row(31, "Linux-tidy-results-2-1", "2026-10-11T00:00:00Z", 6),
            row(32, "Linux-tidy-results-3-1", "2026-10-12T00:00:00Z", 7),
        ]
        self.assertEqual(
            pruner.stale_entries(rows),
            [
                ("Linux-tidy-results", "2026-10-11T00:00:00Z", "31", "6", "Linux-tidy-results-2-1"),
                ("Linux-tidy-results", "2026-10-10T00:00:00Z", "30", "5", "Linux-tidy-results-1-1"),
            ],
        )

    def test_same_timestamp_keeps_the_higher_id(self):
        rows = [
            row(41, "Linux-tidy-results-1-1", "2026-10-10T00:00:00Z"),
            row(40, "Linux-tidy-results-2-1", "2026-10-10T00:00:00Z"),
        ]
        self.assertEqual(stale_keys(rows), ["Linux-tidy-results-2-1"])

    def test_malformed_line_is_an_error(self):
        with self.assertRaises(ValueError):
            pruner.stale_entries(["1\tkey-only"])

    def test_command_line(self):
        rows = [
            row(1, "Windows-cmake-checks-win-debug-0000000000000001-v1", "2026-10-16T10:00:00Z"),
            row(2, "Windows-cmake-checks-win-debug-0000000000000002-v1", "2026-10-17T10:00:00Z"),
        ]
        result = subprocess.run(
            [sys.executable, "-I", str(SCRIPT)],
            input="\n".join(rows) + "\n",
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(
            result.stdout,
            "Windows-cmake-checks-win-debug\t2026-10-16T10:00:00Z\t1\t1000\t"
            "Windows-cmake-checks-win-debug-0000000000000001-v1\n",
        )


class RepoKeyTests(unittest.TestCase):
    """The keys the repo actually saves keep the architecture where the family can see it (#2528)."""

    def test_cmake_check_cache_key_names_a_non_x64_architecture_before_cmake_checks(self):
        action = (REPO / ".github" / "actions" / "cmake-check-cache" / "action.yml").read_text(encoding="utf-8")
        match = re.search(r'key="\$\{RUNNER_OS\}-\$\{(\w+)\}cmake-checks-', action)
        self.assertIsNotNone(match, "cmake-check-cache's key no longer has an architecture part before cmake-checks")
        self.assertRegex(action, rf'{match.group(1)}="\$\{{RUNNER_ARCH\}}-"')

    def test_build_ccache_key_names_a_non_x64_architecture(self):
        workflow = (REPO / ".github" / "workflows" / "reusable-build-test.yml").read_text(encoding="utf-8")
        arch_part = "${{ runner.arch != 'X64' && format('{0}-', runner.arch) || '' }}"
        self.assertIn("key: ${{ runner.os }}-" + arch_part, workflow)

    def test_prune_workflow_uses_this_script(self):
        workflow = (REPO / ".github" / "workflows" / "cache-prune.yml").read_text(encoding="utf-8")
        self.assertIn("tools/cache-prune-stale.py", workflow)


if __name__ == "__main__":
    unittest.main()
