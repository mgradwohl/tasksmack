#!/usr/bin/env python3
"""Tests for tools/arm64-paths.py (#3930): the globs under the labeler's `ci:arm64` key decide whether a
change runs ARM64 CI, matched the way the labeler matches them."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "arm64-paths.py"
spec = importlib.util.spec_from_file_location("arm64_paths", SCRIPT)
arm64_paths = importlib.util.module_from_spec(spec)
spec.loader.exec_module(arm64_paths)

LABELER = """\
# comment
"windows":
  - changed-files:
    - any-glob-to-any-file:
      - "src/Platform/Windows/**"

# a comment between blocks
"ci:arm64":
  - changed-files:
    - any-glob-to-any-file:
      - ".github/actions/**"
      - "CMakeLists.txt"
      - "vcpkg*.json"   # trailing comment
      - "src/Platform/Windows/WindowsProcessMachine*"
      - "**/arm.txt"

"tooling":
  - changed-files:
    - any-glob-to-any-file:
      - "tools/**"
"""


class GlobsForLabel(unittest.TestCase):
    def test_reads_only_the_label_block(self) -> None:
        self.assertEqual(
            arm64_paths.globs_for_label(LABELER),
            [".github/actions/**", "CMakeLists.txt", "vcpkg*.json", "src/Platform/Windows/WindowsProcessMachine*", "**/arm.txt"],
        )

    def test_missing_label_gives_none(self) -> None:
        self.assertEqual(arm64_paths.globs_for_label(LABELER, "nope"), [])


class Matching(unittest.TestCase):
    GLOBS = arm64_paths.globs_for_label(LABELER)

    def test_matches(self) -> None:
        for path in (
            ".github/actions/setup-llvm/action.yml",
            "CMakeLists.txt",
            "vcpkg.json",
            "vcpkg-configuration.json",
            "src/Platform/Windows/WindowsProcessMachine.cpp",
            "arm.txt",
            "a/b/arm.txt",
        ):
            with self.subTest(path):
                self.assertTrue(arm64_paths.matches([path], self.GLOBS))

    def test_does_not_match(self) -> None:
        for path in (
            "src/CMakeLists.txt",  # a bare name is top-level only
            "docs/vcpkg/x.json",
            "src/Platform/Windows/WindowsProcessList.cpp",
            ".github/workflows/ci.yml",
            "README.md",
            "xarm.txt",
        ):
            with self.subTest(path):
                self.assertFalse(arm64_paths.matches([path], self.GLOBS))

    def test_one_matching_path_among_others_is_enough(self) -> None:
        self.assertTrue(arm64_paths.matches(["README.md", "CMakeLists.txt"], self.GLOBS))

    def test_no_paths_matches_nothing(self) -> None:
        self.assertFalse(arm64_paths.matches([], self.GLOBS))


class CommandLine(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.labeler = self.root / "labels.txt"
        self.labeler.write_text(LABELER, encoding="utf-8")

    def run_script(self, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--labeler", str(self.labeler), *args],
            capture_output=True,
            text=True,
            cwd=cwd,
        )

    def test_paths(self) -> None:
        hit = self.run_script("--paths", "README.md", "CMakeLists.txt")
        self.assertEqual((hit.returncode, hit.stdout.strip()), (0, "true"))
        miss = self.run_script("--paths", "README.md")
        self.assertEqual((miss.returncode, miss.stdout.strip()), (0, "false"))

    def test_no_globs_exits_2_without_output(self) -> None:
        self.labeler.write_text('"windows":\n  - "x"\n', encoding="utf-8")
        result = self.run_script("--paths", "CMakeLists.txt")
        self.assertEqual((result.returncode, result.stdout), (2, ""))

    def test_missing_labeler_exits_2(self) -> None:
        self.labeler.unlink()
        self.assertEqual(self.run_script("--paths", "x").returncode, 2)

    def git(self, *args: str) -> None:
        subprocess.run(
            ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false", *args],
            cwd=self.root / "repo",
            check=True,
            capture_output=True,
        )

    def test_git_diff_against_base(self) -> None:
        repo = self.root / "repo"
        repo.mkdir()
        self.git("init", "-q")
        (repo / "README.md").write_text("a\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "base")
        self.git("tag", "base")
        (repo / "README.md").write_text("b\n", encoding="utf-8")
        self.git("commit", "-q", "-am", "docs")
        self.assertEqual(self.run_script("--base", "base", cwd=repo).stdout.strip(), "false")
        (repo / "CMakeLists.txt").write_text("x\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "build")
        self.assertEqual(self.run_script("--base", "base", cwd=repo).stdout.strip(), "true")

    def test_git_failure_exits_2(self) -> None:
        result = self.run_script("--base", "no-such-commit", cwd=self.root)
        self.assertEqual((result.returncode, result.stdout), (2, ""))


if __name__ == "__main__":
    unittest.main()
