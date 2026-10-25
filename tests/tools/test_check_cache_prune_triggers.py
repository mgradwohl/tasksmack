#!/usr/bin/env python3
"""Tests for tools/check-cache-prune-triggers.py (#2410): every workflow that saves Actions caches must be
in cache-prune.yml's workflow_run list, or its superseded entries are only pruned when some other listed
workflow happens to finish after it."""

import importlib.util
import shutil
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check-cache-prune-triggers.py"
spec = importlib.util.spec_from_file_location("check_cache_prune_triggers", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

REUSABLE = """\
name: Reusable Build and Test
on:
  workflow_call:
    inputs:
      os:
        type: string
jobs:
  build:
    runs-on: ${{ inputs.os }}
    steps:
      - uses: hendrikmuhs/ccache-action@0000000000000000000000000000000000000000 # v1
"""

LLVM_ACTION = """\
name: Setup Windows LLVM
inputs:
  llvm-version:
    required: true
runs:
  using: composite
  steps:
    - uses: actions/cache/save@0000000000000000000000000000000000000000 # v6
      with:
        key: windows-llvm
"""

MODE_ACTION = """\
name: FetchContent cache
inputs:
  mode:
    required: true
runs:
  using: composite
  steps:
    - if: inputs.mode == 'save'
      uses: actions/cache/save@0000000000000000000000000000000000000000 # v6
"""

CALL_REUSABLE = "- uses: ./.github/workflows/reusable-build-test.yml\n"


def prune(names: str) -> str:
    return f"""\
name: Cache Prune
on:
  workflow_run:
    workflows: {names}
    types: [completed]
jobs: {{}}
"""


def workflow(name: str, steps: str, on: str = "push:\n    branches: [main]") -> str:
    header = f"name: {name}\non:\n  {on}\njobs:\n  job:\n    runs-on: ubuntu-24.04\n    steps:\n"
    return header + textwrap.indent(steps, "      ")


class CheckCachePruneTriggersTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="cache-prune-triggers-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.write(".github/workflows/reusable-build-test.yml", REUSABLE)
        self.write(".github/actions/setup-windows-llvm/action.yml", LLVM_ACTION)
        self.write(".github/actions/fetchcontent-cache/action.yml", MODE_ACTION)
        self.write(".github/workflows/ci.yml", workflow("CI", CALL_REUSABLE))

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def errors(self) -> list[str]:
        return checker.check(self.root)

    def test_listed_caller_of_saving_reusable_workflow_passes(self) -> None:
        self.write(".github/workflows/cache-prune.yml", prune("[CI]"))
        self.assertEqual(self.errors(), [])

    def test_unlisted_caller_of_saving_reusable_workflow_fails(self) -> None:
        # The #2410 regression: ARM64 calls reusable-build-test.yml but is missing from the list.
        self.write(".github/workflows/cache-prune.yml", prune("[CI]"))
        self.write(
            ".github/workflows/arm64.yml",
            workflow("ARM64", CALL_REUSABLE, on="schedule:\n    - cron: '0 7 * * *'"),
        )
        errors = self.errors()
        self.assertEqual(len(errors), 1)
        self.assertIn("arm64.yml", errors[0])
        self.assertIn("'ARM64'", errors[0])

    def test_block_style_list_is_read(self) -> None:
        self.write(".github/workflows/cache-prune.yml", prune("\n      - CI\n      - 'ARM64'  # quoted"))
        self.write(".github/workflows/arm64.yml", workflow("ARM64", CALL_REUSABLE))
        self.assertEqual(self.errors(), [])

    def test_each_direct_save_form_is_detected(self) -> None:
        self.write(".github/workflows/cache-prune.yml", prune("[CI]"))
        cases = {
            "ccache": "- uses: hendrikmuhs/ccache-action@0000000000000000000000000000000000000000 # v1\n",
            "cache": "- uses: actions/cache@0000000000000000000000000000000000000000 # v6\n",
            "cache-save": "- uses: actions/cache/save@0000000000000000000000000000000000000000 # v6\n",
            "mode-save": "- uses: ./.github/actions/fetchcontent-cache\n  with:\n    mode: save\n",
            "llvm": "- uses: ./.github/actions/setup-windows-llvm\n  with:\n    llvm-version: '22'\n",
        }
        for name, steps in cases.items():
            with self.subTest(name=name):
                self.write(".github/workflows/extra.yml", workflow(f"Extra {name}", steps, on="workflow_dispatch:"))
                errors = self.errors()
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(f"'Extra {name}'", errors[0])

    def test_restores_and_comments_are_not_saves(self) -> None:
        self.write(".github/workflows/cache-prune.yml", prune("[CI]"))
        steps = (
            "- uses: ./.github/actions/fetchcontent-cache\n  with:\n    mode: restore\n"
            "- uses: actions/cache/restore@0000000000000000000000000000000000000000 # v6\n"
            "# - uses: hendrikmuhs/ccache-action@0000000000000000000000000000000000000000\n"
        )
        self.write(".github/workflows/lint.yml", workflow("Lint", steps))
        self.assertEqual(self.errors(), [])

    def test_reusable_workflow_itself_need_not_be_listed(self) -> None:
        self.write(".github/workflows/cache-prune.yml", prune("[]"))
        self.write(".github/workflows/ci.yml", workflow("CI", "- run: true\n"))
        self.assertEqual(self.errors(), [])

    def test_missing_list_fails(self) -> None:
        self.write(".github/workflows/cache-prune.yml", "name: Cache Prune\non:\n  workflow_dispatch:\njobs: {}\n")
        self.assertEqual(len(self.errors()), 1)

    def test_repository_workflows_pass(self) -> None:
        self.assertEqual(checker.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
