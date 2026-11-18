#!/usr/bin/env python3
"""Tests for tools/check-no-reverts.py, the revert guard (#3986, after #3972), on scratch git repositories.

A change whose diff removes lines a recent merge added fails, as #3959 and #3952 did when stale copies of files
reverted six merged PRs. A move to another file, a reformat, an older line that only matches a recent one's text,
an exempt lockfile, and an overridden finding pass."""

import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
spec = importlib.util.spec_from_file_location("check_no_reverts", TOOLS / "check-no-reverts.py")
check_no_reverts = importlib.util.module_from_spec(spec)
sys.modules["check_no_reverts"] = check_no_reverts  # dataclasses look their module up by name
spec.loader.exec_module(check_no_reverts)

GIT_ENV = {
    "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
    "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
}

# Long enough to count (over 25 characters), and distinct.
def code(tag: str, count: int) -> str:
    return "".join(f"    int {tag}_value_{i} = compute_the_{tag}_thing({i});\n" for i in range(count))


@unittest.skipUnless(shutil.which("git"), "git not available")
class CheckNoRevertsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Path(tempfile.mkdtemp(prefix="check-no-reverts-"))
        self.addCleanup(shutil.rmtree, self.repo, ignore_errors=True)
        self.git("init", "-q", "-b", "main")
        self.commit("the start", {"README.md": "TaskSmack\n", "src/a.cpp": code("old", 10)})

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], capture_output=True, text=True, check=True,
                              env={**os.environ, **GIT_ENV}).stdout

    def commit(self, message: str, files: dict[str, str | None]) -> str:
        for name, text in files.items():
            path = self.repo / name
            if text is None:
                path.unlink()
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD").strip()

    def branch(self, name: str, at: str = "main") -> None:
        self.git("switch", "-q", "-c", name, at)

    def run_check(self, head: str, *extra: str, base: str = "main") -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code_ = check_no_reverts.main(["--repo", str(self.repo), "--base", base, "--head", head, *extra])
        return code_, out.getvalue(), err.getvalue()

    def merged(self, message: str, files: dict[str, str | None]) -> str:
        """A squash merge on main."""
        self.git("switch", "-q", "main")
        return self.commit(message, files)

    # ---- what fails ----------------------------------------------------------------------------------------------

    def test_a_stale_copy_that_reverts_a_recent_merge_fails(self) -> None:
        # #3972's shape: the branch's copy of a.cpp predates #101, so its diff takes #101's lines back out.
        before = self.git("show", "HEAD:src/a.cpp")
        self.merged("feat: the new lines (#101)", {"src/a.cpp": before + code("new", 8)})
        self.branch("stale")
        head = self.commit("fix: something else, over a stale tree (#102)", {"src/a.cpp": before, "src/b.cpp": code("b", 3)})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 1, out)
        self.assertIn("feat: the new lines (#101)", out)
        self.assertIn("| 8 |", out)
        self.assertIn("**fails**", out)
        self.assertIn("::error title=Revert guard::This change removes 8 line(s)", out)

    def test_deleting_a_file_a_recent_merge_created_fails(self) -> None:
        self.merged("ci: a new workflow (#103)", {"warm.yml": code("warm", 9)})
        self.branch("gone")
        head = self.commit("fix: unrelated (#104)", {"warm.yml": None, "src/c.cpp": code("c", 2)})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 1, out)
        self.assertIn("ci: a new workflow (#103)", out)

    def test_a_merge_group_that_reverts_a_merged_pr_fails(self) -> None:
        # #3818: a merge group's head against its base_sha. The queued change reverts #105, merged before the
        # group's base; the group's other PR (#106) is clean.
        before = self.git("show", "HEAD:src/a.cpp")
        self.merged("feat: queued-before lines (#105)", {"src/a.cpp": before + code("q", 7)})
        base = self.git("rev-parse", "HEAD").strip()
        self.branch("gh-readonly-queue/main/pr-107")
        self.commit("feat: clean (#106)", {"src/d.cpp": code("d", 4)})
        head = self.commit("fix: stale (#107)", {"src/a.cpp": before})
        status, out, _ = self.run_check(head, base=base)
        self.assertEqual(status, 1, out)
        self.assertIn("(#105)", out)

    # ---- what passes ---------------------------------------------------------------------------------------------

    def test_moving_recent_code_to_another_file_passes(self) -> None:
        # #3989, #3812: a refactor moves code merged yesterday into a file of its own.
        before = self.git("show", "HEAD:src/a.cpp")
        moved = code("moving", 12)
        self.merged("feat: the code to move (#110)", {"src/a.cpp": before + moved})
        self.branch("split")
        head = self.commit("refactor: split it out (#111)", {"src/a.cpp": before, "src/moved.cpp": "// split\n" + moved})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 0, out)
        self.assertIn("12 removed line(s) counted as moved", out)
        self.assertIn("src/a.cpp -> src/moved.cpp", out)

    def test_a_reformat_of_recent_lines_passes(self) -> None:
        before = self.git("show", "HEAD:src/a.cpp")
        recent = code("fmt", 8)
        self.merged("feat: lines clang-format will reflow (#112)", {"src/a.cpp": before + recent})
        self.branch("reflow")
        reflowed = "".join("  " + line.strip().replace("= ", "=  ") + "\n" for line in recent.splitlines())
        head = self.commit("style: reformat (#113)", {"src/a.cpp": before + reflowed})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 0, out)

    def test_old_lines_that_match_a_recent_commits_text_pass(self) -> None:
        # Blame, not text: the removed lines are the start's, older than the window, though #115 added the same
        # text in another file.
        old = self.git("show", "HEAD:src/a.cpp")
        for n in range(3):
            self.merged(f"chore: filler {n} (#12{n})", {f"src/filler{n}.cpp": code(f"f{n}", 1)})
        self.merged("feat: the same text elsewhere (#115)", {"src/copy.cpp": old})
        self.branch("old")
        head = self.commit("refactor: drop the old code (#116)", {"src/a.cpp": "// gone\n"})
        status, out, _ = self.run_check(head, "--window", "4")
        self.assertEqual(status, 0, out)
        # Inside a wider window the start's own lines count: they are the ones removed.
        status, out, _ = self.run_check(head, "--window", "10")
        self.assertEqual(status, 1, out)
        self.assertIn("the start", out)

    def test_an_exempt_lockfile_passes(self) -> None:
        self.merged("deps: a pin (#117)", {"vcpkg.json": code("pin", 8)})
        self.branch("pins")
        head = self.commit("deps: roll the pin (#118)", {"vcpkg.json": code("other", 8)})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 0, out)

    def test_short_lines_and_small_removals_pass(self) -> None:
        before = self.git("show", "HEAD:src/a.cpp")
        self.merged("feat: a few lines (#119)", {"src/a.cpp": before + code("few", 5) + "}\n}\n}\n"})
        self.branch("small")
        head = self.commit("fix: trim (#120)", {"src/a.cpp": before})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 0, out)  # 5 long lines, under 6; the braces are too short to count

    # ---- overrides, advisory and errors -------------------------------------------------------------------------

    def stale_revert(self) -> str:
        before = self.git("show", "HEAD:src/a.cpp")
        self.merged("feat: lines (#130)", {"src/a.cpp": before + code("x", 8)})
        self.merged("feat: more lines (#131)", {"src/e.cpp": code("y", 8)})
        self.branch("revert")
        return self.commit("revert: both (#132)", {"src/a.cpp": before, "src/e.cpp": None})

    def test_the_label_clears_every_finding(self) -> None:
        head = self.stale_revert()
        status, out, _ = self.run_check(head, "--labels", "bug, ci:revert-ok")
        self.assertEqual(status, 0, out)
        self.assertEqual(out.count("cleared: the ci:revert-ok label"), 2)
        self.assertNotIn("::error", out)

    def test_a_description_naming_the_pr_clears_that_commit_only(self) -> None:
        head = self.stale_revert()
        body = self.repo / "body.md"
        body.write_text("Reverts #130: it broke the build.\n", encoding="utf-8")
        status, out, _ = self.run_check(head, "--body-file", str(body))
        self.assertEqual(status, 1, out)
        self.assertIn("cleared: the description names #130", out)
        self.assertIn("::error title=Revert guard::This change removes 8 line(s) that", out)
        self.assertIn("(#131)", out.split("::error", 1)[1])

    def test_advisory_warns_and_passes_and_writes_the_summary(self) -> None:
        head = self.stale_revert()
        report = self.repo / "summary.md"
        status, out, _ = self.run_check(head, "--advisory", "--summary", str(report))
        self.assertEqual(status, 0, out)
        self.assertIn("::warning title=Revert guard::", out)
        self.assertNotIn("::error", out)
        text = report.read_text(encoding="utf-8")
        self.assertIn("## Revert guard (#3986)", text)
        self.assertIn("| advisory |", text)

    def test_a_stale_copy_of_a_non_ascii_file_still_fails(self) -> None:
        # #4331: the diff header was "--- \"a/docs/guide/caf\\303\\251.md\"", which parse_diff skipped, so the revert
        # passed unchecked. The subject's é must also come back as itself, not as its UTF-8 bytes read in a code page.
        self.merged("docs: the café guide (#150)", {"docs/guide/café.md": code("cafe", 8)})
        self.branch("stale-cafe")
        head = self.commit("fix: unrelated (#151)", {"docs/guide/café.md": "", "src/d.cpp": code("d", 2)})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 1, out)
        self.assertIn("docs: the café guide (#150)", out)
        self.assertIn("docs/guide/café.md", out)

    def test_header_path_unquotes_what_git_still_quotes(self) -> None:
        self.assertEqual(check_no_reverts.header_path("--- a/src/x.cpp", "a"), "src/x.cpp")
        self.assertIsNone(check_no_reverts.header_path("--- /dev/null", "a"))
        self.assertEqual(check_no_reverts.header_path('+++ "b/odd\\tname.md"', "b"), "odd\tname.md")
        self.assertEqual(check_no_reverts.header_path('--- "a/caf\\303\\251.md"', "a"), "café.md")
        self.assertEqual(check_no_reverts.header_path('--- "a/q\\"uote.md"', "a"), 'q"uote.md')

    def test_a_clean_change_passes(self) -> None:
        self.branch("clean")
        head = self.commit("feat: new file (#140)", {"src/new.cpp": code("n", 10)})
        status, out, _ = self.run_check(head)
        self.assertEqual(status, 0, out)
        self.assertIn("No removed lines attributed to a recent merge.", out)

    def test_unusable_history_is_exit_2_or_an_advisory_warning(self) -> None:
        status, _, err = self.run_check("0" * 40)
        self.assertEqual(status, 2)
        self.assertIn("ERROR:", err)
        status, _, err = self.run_check("0" * 40, "--advisory")
        self.assertEqual(status, 0)
        self.assertIn("::warning title=Revert guard::not checked", err)


if __name__ == "__main__":
    unittest.main()
