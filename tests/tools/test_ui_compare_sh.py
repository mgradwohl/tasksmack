#!/usr/bin/env python3
"""Tests for tools/ui-compare.sh's argument checks (#4213) and dependency seeding (#4214): the script is
sourced, so its functions run on fixture trees and throwaway git repositories, and nothing is built. Needs
bash and git; Linux only, as the script is."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "ui-compare.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"


def git(cwd: Path, *args: str) -> str:
    env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True, capture_output=True, text=True).stdout.strip()


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("git"), "Linux, with git")
class UiCompareShTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="ui_compare_sh_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def call(self, snippet: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        """Source the script (it only defines its functions then) and run @p snippet."""
        return subprocess.run([BASH, "-c", f'source "{SCRIPT}"; {snippet}'], capture_output=True, text=True,
                              env={**os.environ, **(env or {})}, check=False, timeout=60)

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([BASH, str(SCRIPT), *args], capture_output=True, text=True, check=False, timeout=60)

    def test_an_option_without_its_value_prints_the_usage(self):
        # #4213: set -u stopped on "$2: unbound variable" (exit 1).
        for option in ("--out", "--seed", "--work"):
            with self.subTest(option=option):
                result = self.run_script(option)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(f"{option} requires a value", result.stderr)
                self.assertIn("Usage:", result.stderr)
                self.assertNotIn("unbound variable", result.stderr)

    def test_the_default_seed_is_where_this_checkout_put_its_sources(self):
        # #4214: cmake/Dependencies.cmake uses FETCHCONTENT_BASE_DIR when it is set.
        result = self.call('echo "$SEED"', env={"FETCHCONTENT_BASE_DIR": "/elsewhere/cache"})
        self.assertEqual(result.stdout.strip(), "/elsewhere/cache", result.stderr)
        env = {key: value for key, value in os.environ.items() if key != "FETCHCONTENT_BASE_DIR"}
        result = subprocess.run([BASH, "-c", f'source "{SCRIPT}"; echo "$SEED"'], capture_output=True, text=True,
                                env=env, check=False, timeout=60)
        self.assertEqual(result.stdout.strip(), f"{REPO_ROOT}/.cache/fetchcontent", result.stderr)

    def tree_pinning(self, name: str, sha: str) -> Path:
        tree = self.root / f"tree-{sha[:6]}"
        (tree / "cmake").mkdir(parents=True)
        (tree / "cmake" / "Dependencies.cmake").write_text(textwrap.dedent(f"""\
            FetchContent_Declare(
                {name}
                # the version comment the bump updates
                GIT_REPOSITORY https://example.invalid/{name}.git
                GIT_TAG {sha} # v1.0
            )
            """), encoding="utf-8")
        return tree

    def test_declared_pins_reads_each_dependencys_tag(self):
        tree = self.tree_pinning("ImGui", "0123456789abcdef0123456789abcdef01234567")
        result = self.call(f'declared_pins "{tree}"')
        self.assertEqual(result.stdout.split(), ["imgui", "0123456789abcdef0123456789abcdef01234567"], result.stderr)

    def test_a_dependency_is_seeded_only_at_the_seeds_own_commit(self):
        seed = self.root / "seed"
        source = seed / "imgui-src"
        source.mkdir(parents=True)
        git(source, "init", "-q")
        (source / "imgui.h").write_text("// x\n", encoding="utf-8")
        git(source, "add", ".")
        git(source, "commit", "-q", "-m", "seed")
        head = git(source, "rev-parse", "HEAD")
        same = self.call(f'SEED="{seed}"; seed_arguments "{self.tree_pinning("imgui", head)}"')
        self.assertEqual(same.stdout.strip(), f"-DFETCHCONTENT_SOURCE_DIR_IMGUI={source}", same.stderr)
        bumped = self.call(f'SEED="{seed}"; seed_arguments "{self.tree_pinning("imgui", "f" * 40)}"')
        self.assertEqual(bumped.stdout.strip(), "", "the bumped dependency is downloaded, not seeded")
        self.assertIn("isn't the seed's checkout", bumped.stderr)

    def test_a_ref_with_other_patches_isnt_seeded(self):
        # #4214: a seeded source keeps this checkout's patches, so a ref whose patches differ must not reuse it.
        tree = self.root / "tree"
        shutil.copytree(REPO_ROOT / "cmake" / "patches", tree / "cmake" / "patches")
        self.assertEqual(self.call(f'patches_match "{tree}"').returncode, 0, "the same patches")
        next(path for path in (tree / "cmake" / "patches").rglob("*") if path.is_file()).write_text("changed\n", encoding="utf-8")
        self.assertNotEqual(self.call(f'patches_match "{tree}"').returncode, 0, "a changed patch")

    def test_each_side_configures_with_its_own_dependency_cache(self):
        # #4214: never an inherited FETCHCONTENT_BASE_DIR, which would change the user's other builds' sources.
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('seeds+=("-DTASKSMACK_FETCHCONTENT_CACHE_DIR=$tree/.cache/fetchcontent")', text)


if __name__ == "__main__":
    unittest.main()
