#!/usr/bin/env python3
"""Tests for tools/tidy-cache.py: the per-file clang-tidy result cache (#1785). A key must change
whenever anything that can change clang-tidy's result for a file changes, so a cached pass is never
reused for different inputs; and every failure must fall back to analyzing the file."""

import argparse
import contextlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "tidy-cache.py"
spec = importlib.util.spec_from_file_location("tidy_cache", SCRIPT)
tidy_cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tidy_cache)


class TidyCacheTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="tidy-cache-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write("src/A.cpp", '#include "A.h"\nint a() { return 1; }\n')
        self.write("src/A.h", "#pragma once\nint a();\n")
        self.write("src/B.cpp", "int b() { return 2; }\n")
        self.write(".clang-tidy", "Checks: '*'\n")
        self.compdb = self.root / "build" / "compile_commands.json"
        self.compdb.parent.mkdir()
        (self.root / ".gitignore").write_text("build/\ncache/\n", encoding="utf-8")
        self.set_compdb("-DX=1")
        self.cache = self.root / "cache"
        # Dependencies "scanned" for each file: by default A.cpp includes A.h.
        self.deps = {"src/A.cpp": ["src/A.cpp", "src/A.h"], "src/B.cpp": ["src/B.cpp"]}
        self.scan_fails = False
        self.scanned_args: list[str] = []

        def fake_scan(_scan_deps: str, entries: list[dict]) -> dict[str, list[str]]:
            if self.scan_fails:
                return {}
            self.scanned_args = [e.get("command", "") for e in entries]
            result = {}
            for entry in entries:
                rel = os.path.relpath(entry["file"], self.root).replace("\\", "/")
                result[tidy_cache.norm(entry["file"])] = [str(self.root / d) for d in self.deps[rel]]
            return result

        patches = {"scan_dependencies": fake_scan, "find_scan_deps": lambda _tidy: "clang-scan-deps"}
        for name, value in patches.items():
            original = getattr(tidy_cache, name)
            setattr(tidy_cache, name, value)
            self.addCleanup(setattr, tidy_cache, name, original)

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def set_compdb(self, define: str) -> None:
        entries = [
            {"directory": str(self.root / "build"), "file": str(self.root / f"src/{n}.cpp"),
             "command": f"clang++ {define} -c ../src/{n}.cpp"}
            for n in ("A", "B")
        ]
        self.compdb.write_text(json.dumps(entries), encoding="utf-8")

    def select(self, *extra: str, salt: tuple[str, ...] = (), prune: bool = False) -> dict[str, str]:
        """Runs select on both files; returns {relative file: key} for the files to analyze."""
        args = argparse.Namespace(
            cache_dir=str(self.cache), compdb=str(self.compdb), clang_tidy=sys.executable,
            repo_root=str(self.root), salt=list(salt), salt_file=[str(self.root / ".clang-tidy")],
            extra_arg=list(extra) or ["-std=c++23"], prune=prune, files_from=None,
            files=[str(self.root / "src/A.cpp"), str(self.root / "src/B.cpp")],
        )
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(tidy_cache.select(args), 0)
        result = {}
        for line in out.getvalue().splitlines():
            key, path = line.split("\t", 1)
            result[os.path.relpath(path, self.root).replace("\\", "/")] = key
        return result

    def record(self, todo: dict[str, str]) -> None:
        """What the tidy scripts do after a clean run: write <cache>/<key>."""
        for key in todo.values():
            self.assertRegex(key, r"^[0-9a-f]{64}$")
            (self.cache / key).write_text("clean\n", encoding="utf-8")

    def test_clean_files_are_skipped_next_time(self) -> None:
        first = self.select()
        self.assertEqual(set(first), {"src/A.cpp", "src/B.cpp"})
        self.record(first)
        self.assertEqual(self.select(), {})

    def test_unrecorded_file_is_analyzed_again(self) -> None:
        first = self.select()
        self.record({"src/B.cpp": first["src/B.cpp"]})  # A.cpp had findings: not recorded
        self.assertEqual(set(self.select()), {"src/A.cpp"})

    def test_included_header_change_invalidates_only_its_includers(self) -> None:
        self.record(self.select())
        self.write("src/A.h", "#pragma once\nint a(); // NOLINT\n")  # a comment counts
        self.assertEqual(set(self.select()), {"src/A.cpp"})

    def test_compile_command_change_invalidates(self) -> None:
        self.record(self.select())
        self.set_compdb("-DX=2")
        self.assertEqual(set(self.select()), {"src/A.cpp", "src/B.cpp"})

    def test_config_salt_and_extra_args_invalidate(self) -> None:
        self.record(self.select())
        self.write(".clang-tidy", "Checks: '-*'\n")
        self.assertEqual(set(self.select()), {"src/A.cpp", "src/B.cpp"})
        self.record(self.select())
        self.assertEqual(set(self.select(salt=("--header-filter=x",))), {"src/A.cpp", "src/B.cpp"})
        self.assertEqual(set(self.select("-std=c++26")), {"src/A.cpp", "src/B.cpp"})
        self.assertTrue(all(c.endswith("-std=c++26") for c in self.scanned_args))

    def test_runner_image_change_invalidates(self) -> None:
        self.record(self.select())
        old = os.environ.get("ImageVersion")
        os.environ["ImageVersion"] = "test-image-2"
        self.addCleanup(lambda: os.environ.pop("ImageVersion") if old is None else os.environ.update(ImageVersion=old))
        self.assertEqual(set(self.select()), {"src/A.cpp", "src/B.cpp"})

    def test_new_file_that_could_shadow_an_include_invalidates(self) -> None:
        self.record(self.select())
        self.write("src/Other/A.h", "#pragma once\n")  # untracked files count too
        self.assertEqual(set(self.select()), {"src/A.cpp"})

    def test_scan_failure_analyzes_everything_unkeyed(self) -> None:
        self.record(self.select())
        self.scan_fails = True
        self.assertEqual(self.select(), {"src/A.cpp": "-", "src/B.cpp": "-"})

    def test_missing_dependency_is_not_cached(self) -> None:
        self.deps["src/A.cpp"].append("src/Gone.h")
        todo = self.select()
        self.assertEqual(todo["src/A.cpp"], "-")
        self.assertNotEqual(todo["src/B.cpp"], "-")

    def test_prune_keeps_only_current_keys(self) -> None:
        self.record(self.select())
        stale = self.cache / ("0" * 64)
        stale.write_text("old\n", encoding="utf-8")
        other = self.cache / "README"
        other.write_text("not a marker\n", encoding="utf-8")
        self.select()
        self.assertTrue(stale.exists())
        self.select(prune=True)
        self.assertFalse(stale.exists())
        self.assertTrue(other.exists())
        self.assertEqual(len([p for p in self.cache.iterdir() if p.name != "README"]), 2)


if __name__ == "__main__":
    unittest.main()
