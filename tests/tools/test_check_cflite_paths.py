#!/usr/bin/env python3
"""Tests for tools/check-cflite-paths.py (#2533): cflite_build.yml must refresh the fuzzing baseline for
every path cflite_pr.yml fuzzes, and the other way round."""

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check-cflite-paths.py"
spec = importlib.util.spec_from_file_location("check_cflite_paths", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def pr_workflow(paths: list[str]) -> str:
    items = "".join(f"      - {p}\n" for p in paths)
    return (
        "name: ClusterFuzzLite PR Fuzzing\n\non:\n  pull_request:\n    branches: [main]\n"
        f"    # a comment inside the trigger\n    paths:\n{items}\n"
        "concurrency:\n  group: cflite-pr\n  cancel-in-progress: true\n\n"
        "jobs:\n  fuzz:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: 'true'\n"
    )


def build_workflow(paths: list[str]) -> str:
    items = "".join(f"      - {p}\n" for p in paths)
    return (
        "name: ClusterFuzzLite Continuous Build\n\non:\n  push:\n    branches: [main]\n"
        f"    paths:\n{items}  workflow_dispatch:\n\n"
        "jobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n"
        "      - uses: some/action@0000000000000000000000000000000000000000\n        with:\n"
        "          paths:\n            - not-a-trigger-path\n"
    )


class CheckCflitePathsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="cflite-paths-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write(self, name: str, text: str) -> None:
        path = self.root / ".github" / "workflows" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def errors(self) -> list[str]:
        return checker.check(self.root)

    def test_matching_lists_pass_regardless_of_order_and_quoting(self) -> None:
        self.write("cflite_pr.yml", pr_workflow(["'tests/fuzz/**'", "'src/Platform/DnsMessage.h'"]))
        self.write("cflite_build.yml", build_workflow(['"src/Platform/DnsMessage.h"', "tests/fuzz/**  # fuzz"]))
        self.assertEqual(self.errors(), [])

    def test_path_missing_from_build_fails(self) -> None:
        # The #2533 regression: PR fuzzing watches a parser the baseline build doesn't.
        self.write("cflite_pr.yml", pr_workflow(["'tests/fuzz/**'", "'src/Platform/DnsMessage.h'"]))
        self.write("cflite_build.yml", build_workflow(["'tests/fuzz/**'"]))
        errors = self.errors()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("cflite_build.yml", errors[0])
        self.assertIn("'src/Platform/DnsMessage.h'", errors[0])

    def test_path_missing_from_pr_fails(self) -> None:
        self.write("cflite_pr.yml", pr_workflow(["'tests/fuzz/**'"]))
        self.write("cflite_build.yml", build_workflow(["'tests/fuzz/**'", "'cmake/patches/**'"]))
        errors = self.errors()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("cflite_pr.yml", errors[0])
        self.assertIn("'cmake/patches/**'", errors[0])

    def test_step_inputs_named_paths_are_not_the_trigger_list(self) -> None:
        # build_workflow's step has a `paths:` input; only the one under `on: push:` counts.
        self.write("cflite_pr.yml", pr_workflow(["'tests/fuzz/**'"]))
        self.write("cflite_build.yml", build_workflow(["'tests/fuzz/**'"]))
        self.assertEqual(self.errors(), [])

    def test_missing_workflow_or_list_is_reported(self) -> None:
        self.write("cflite_pr.yml", "name: PR\non:\n  pull_request:\n    branches: [main]\njobs: {}\n")
        errors = self.errors()
        self.assertEqual(len(errors), 2, errors)
        self.assertIn("cflite_pr.yml: no block-style `paths:` list under `on: pull_request:`", errors[0])
        self.assertIn("cflite_build.yml: missing", errors[1])

    def test_repository_lists_agree(self) -> None:
        self.assertEqual(checker.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
