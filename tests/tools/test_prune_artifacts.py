#!/usr/bin/env python3
"""Tests for tools/prune-artifacts.py's selection (#4617): which superseded artifacts the nightly prune deletes."""

import datetime
import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "prune-artifacts.py"
SPEC = importlib.util.spec_from_file_location("prune_artifacts", SCRIPT)
prune = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prune)

NOW = datetime.datetime(2026, 11, 21, 12, 0, tzinfo=datetime.timezone.utc)


def artifact(aid, name, hours_ago, branch="main", expired=False):
    created = (NOW - datetime.timedelta(hours=hours_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {"id": aid, "name": name, "created_at": created, "branch": branch, "expired": expired}


def ids(chosen):
    return sorted(a["id"] for a in chosen)


class SelectForDeletionTests(unittest.TestCase):
    def test_every_copy_but_the_newest_of_a_name_on_a_branch_goes(self):
        chosen = prune.select_for_deletion(
            [
                artifact(1, "linux-debug-test-results", 30),
                artifact(2, "linux-debug-test-results", 20),
                artifact(3, "linux-debug-test-results", 10),
                artifact(4, "linux-debug-test-results", 40, branch="fix/x"),  # the newest on its own branch
            ],
            NOW,
            6,
        )
        self.assertEqual(ids(chosen), [1, 2])

    def test_a_recent_copy_stays_for_a_run_still_going(self):
        chosen = prune.select_for_deletion(
            [artifact(1, "coverage-report", 2), artifact(2, "coverage-report", 1)],
            NOW,
            6,
        )
        self.assertEqual(chosen, [], "both newer than 6 h")

    def test_benchmark_and_dashboard_data_are_kept_on_every_branch(self):
        chosen = prune.select_for_deletion(
            [
                artifact(1, "benchmark-results", 50),
                artifact(2, "benchmark-results", 30),
                artifact(3, "benchmark-second-runner-results", 50),
                artifact(4, "benchmark-second-runner-results", 30),
                artifact(5, "release-dashboard-data", 50),
                artifact(6, "release-dashboard-data", 30),
            ],
            NOW,
            6,
        )
        self.assertEqual(chosen, [], "the benchmark gate reads main's results; the dashboard its own")

    def test_cifuzz_artifacts_keep_the_newest_of_each_name_whatever_the_branch(self):
        chosen = prune.select_for_deletion(
            [
                artifact(1, "cifuzz-corpus-fuzz_proc_text", 50, branch="main"),
                artifact(2, "cifuzz-corpus-fuzz_proc_text", 30, branch="fix/y"),
                artifact(3, "cifuzz-corpus-fuzz_os_release", 30, branch="fix/y"),
            ],
            NOW,
            6,
        )
        self.assertEqual(ids(chosen), [1], "ClusterFuzzLite looks corpora up by name alone")

    def test_expired_artifacts_are_left_to_github(self):
        chosen = prune.select_for_deletion(
            [artifact(1, "asan-ubsan", 50, expired=True), artifact(2, "asan-ubsan", 30)],
            NOW,
            6,
        )
        self.assertEqual(chosen, [])

    def test_the_oldest_are_deleted_first(self):
        chosen = prune.select_for_deletion(
            [artifact(1, "tsan-report", 10), artifact(2, "tsan-report", 90), artifact(3, "tsan-report", 50), artifact(4, "tsan-report", 1)],
            NOW,
            6,
        )
        self.assertEqual([a["id"] for a in chosen], [2, 3, 1])


if __name__ == "__main__":
    unittest.main()
