#!/usr/bin/env python3
"""Tests for tools/compose-release-notes.py (#3144).

The v0.9.1 release shipped with release.yml's placeholder body ("Release notes follow.") until the
notes were applied by hand. changelog.yml's release-notes job now composes them: the title, the
download guide from .github/release-notes/download-guide.md, git-cliff's section under "## Changes"
and the placeholder's own "Verifying downloads" block, and only while the body is still the
placeholder. These cases cover each part, the hand-written-notes rule and the length cap.
"""

from __future__ import annotations

import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "compose-release-notes.py"
TEMPLATE = REPO / ".github" / "release-notes" / "download-guide.md"
spec = importlib.util.spec_from_file_location("compose_release_notes", SCRIPT)
composer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(composer)

VERIFY = """## Verifying downloads

`SHA256SUMS` lists the SHA-256 of every package. Check the files:

```bash
sha256sum -c --ignore-missing SHA256SUMS
# a comment, not a heading
```

See [Verifying a Release](https://github.com/mgradwohl/tasksmack/blob/main/docs/guide/user-guide.md#verifying-a-release)."""

PLACEHOLDER_BODY = "Release notes follow.\n\n" + VERIFY + "\n"

SECTION = """# Changelog

All notable changes to this project will be documented in this file.

## [0.10.0] - 2026-11-20
### Fixed

- **Windows:** A fix ([#1](https://github.com/mgradwohl/tasksmack/issues/1))

### Infrastructure

- A tool change
"""

PER_FILE_ASSETS = ["TaskSmack-0.10.0-win64.zip", "TaskSmack-0.10.0-win64.zip.bundle"]
SUMS_ASSETS = ["TaskSmack-0.10.0-win64.zip", "SHA256SUMS", "SHA256SUMS.bundle"]


def guide_template() -> str:
    return TEMPLATE.read_text(encoding="utf-8")


class TemplateTests(unittest.TestCase):
    def test_every_field_is_filled_and_the_comment_dropped(self):
        guide = composer.render_guide(guide_template(), "0.10.0", SUMS_ASSETS)
        self.assertTrue(guide.startswith("## Which file should I download?\n"), guide[:80])
        self.assertNotIn("{{", guide)
        self.assertNotIn("<!--", guide)
        self.assertIn("**`TaskSmack-0.10.0-win64.exe`**", guide)
        self.assertIn("`sudo apt install ./TaskSmack-0.10.0-Linux.deb`", guide)
        self.assertIn("`TaskSmack-0.10.0-linux-aarch64.tar.gz` (ARM64)", guide)

    def test_an_unknown_field_is_an_error(self):
        with self.assertRaises(composer.CompositionError):
            composer.render_guide("## Guide {{VERSION}} {{CODENAME}}", "0.10.0", [])

    def test_a_release_with_sha256sums_names_it_and_its_bundle(self):
        guide = composer.render_guide(guide_template(), "0.10.0", SUMS_ASSETS)
        self.assertIn(
            "**You don't need the other files to install.** `SHA256SUMS`, `SHA256SUMS.bundle`, "
            "`*-build-inputs.json` and the `*-sbom.spdx.json` are for checking downloads",
            guide,
        )
        self.assertNotIn("The `.bundle` files", guide)

    def test_a_release_with_a_bundle_per_file_names_the_bundles(self):
        # v0.9.x: every asset has its own <asset>.bundle and there is no SHA256SUMS.
        guide = composer.render_guide(guide_template(), "0.9.2", PER_FILE_ASSETS)
        self.assertIn(
            "**You don't need the other files to install.** The `.bundle` files, `*-build-inputs.json` "
            "and the `*-sbom.spdx.json` are for checking downloads",
            guide,
        )
        self.assertNotIn("SHA256SUMS", guide)


class ChangelogTests(unittest.TestCase):
    def test_the_header_line_and_cliffs_file_header_are_dropped(self):
        lines, _ = composer.changelog_lines(SECTION, "0.10.0")
        text = "\n".join(lines)
        self.assertNotIn("[0.10.0]", text)
        self.assertNotIn("# Changelog", text)
        self.assertNotIn("All notable changes", text)
        self.assertTrue(text.startswith("#### Fixed\n"), text[:40])

    def test_subheadings_are_demoted_one_level(self):
        lines, _ = composer.changelog_lines(SECTION, "0.10.0")
        self.assertEqual([line for line in lines if line.startswith("#")], ["#### Fixed", "#### Infrastructure"])

    def test_a_hash_line_in_a_code_block_is_not_a_heading(self):
        section = "## [0.10.0] - 2026-11-20\n### Fixed\n\n```bash\n# not a heading\n```\n"
        lines, _ = composer.changelog_lines(section, "0.10.0")
        self.assertIn("# not a heading", lines)
        self.assertIn("#### Fixed", lines)

    def test_the_anchor_is_githubs_for_the_header(self):
        _, anchor = composer.changelog_lines(SECTION, "0.10.0")
        # CHANGELOG.md#091---2026-10-27 is how v0.9.1's notes link to their section.
        self.assertEqual(anchor, "0100---2026-11-20")

    def test_a_section_for_another_version_is_an_error(self):
        with self.assertRaises(composer.CompositionError):
            composer.changelog_lines(SECTION, "0.10.1")
        with self.assertRaises(composer.CompositionError):
            composer.changelog_lines("### Fixed\n- no header\n", "0.10.0")


class ComposeTests(unittest.TestCase):
    def compose(self, body: str = PLACEHOLDER_BODY, assets: list[str] | None = None, **kwargs):
        return composer.compose("0.10.0", guide_template(), SECTION, body, assets or SUMS_ASSETS, **kwargs)

    def test_the_parts_come_in_order(self):
        notes, log = self.compose()
        self.assertEqual(log, [])
        headings = [line for line in notes.splitlines() if line.startswith("#") and not line.startswith("# ")]
        self.assertEqual(
            headings,
            ["## Which file should I download?", "## Changes", "#### Fixed", "#### Infrastructure",
             "## Verifying downloads"],
        )
        self.assertTrue(notes.startswith("# TaskSmack v0.10.0\n\n## Which file should I download?\n"))
        self.assertTrue(notes.endswith("\n") and not notes.endswith("\n\n"))

    def test_the_verification_block_is_kept_as_written(self):
        notes, _ = self.compose()
        self.assertTrue(notes.endswith(VERIFY + "\n"), notes[-300:])
        self.assertNotIn(composer.PLACEHOLDER, notes)

    def test_crlf_bodies_keep_their_block(self):
        notes, _ = self.compose(body=PLACEHOLDER_BODY.replace("\n", "\r\n"))
        self.assertTrue(notes.endswith(VERIFY + "\n"))
        self.assertNotIn("\r", notes)

    def test_a_body_without_the_block_is_logged(self):
        notes, log = self.compose(body="Release notes follow.\n")
        self.assertNotIn("## Verifying downloads", notes)
        self.assertEqual(len(log), 1)
        self.assertIn("no '## Verifying downloads' block", log[0])

    def test_notes_over_the_cap_cut_the_changelog_and_link_to_it(self):
        bullets = "\n".join(f"- Change number {n} with some words to take up room" for n in range(2000))
        section = f"## [0.10.0] - 2026-11-20\n### Fixed\n\n{bullets}\n\n### Infrastructure\n\n- Last\n"
        notes, log = composer.compose("0.10.0", guide_template(), section, PLACEHOLDER_BODY, SUMS_ASSETS, limit=20_000)
        self.assertLessEqual(len(notes.encode("utf-8")), 20_000)
        self.assertGreater(len(notes.encode("utf-8")), 19_000)  # cut to fill, not dropped
        self.assertIn("- Change number 0 with", notes)
        self.assertNotIn("- Last", notes)
        self.assertIn(
            "The full list is in [CHANGELOG.md](https://github.com/mgradwohl/tasksmack/blob/main/CHANGELOG.md#0100---2026-11-20)",
            notes,
        )
        # The verification block still ends the page, and nothing was cut mid-line.
        self.assertTrue(notes.endswith(VERIFY + "\n"))
        for line in notes.splitlines():
            if line.startswith("- Change number"):
                self.assertTrue(line.endswith("take up room"), line)
        self.assertEqual(len(log), 1)
        self.assertIn("cut to", log[0])

    def test_the_default_cap_stays_under_githubs_limit(self):
        self.assertLessEqual(composer.MAX_BYTES, 120_000)
        bullets = "\n".join(f"- Change {n} " + "x" * 200 for n in range(1000))  # about 210 KB, like v0.9.0's
        section = f"## [0.10.0] - 2026-11-20\n### Fixed\n\n{bullets}\n"
        notes, log = composer.compose("0.10.0", guide_template(), section, PLACEHOLDER_BODY, SUMS_ASSETS)
        self.assertLessEqual(len(notes.encode("utf-8")), composer.MAX_BYTES)
        self.assertTrue(log)

    def test_notes_under_the_cap_are_not_cut(self):
        notes, log = self.compose(limit=len(self.compose()[0].encode("utf-8")))
        self.assertIn("- A tool change", notes)
        self.assertEqual(log, [])


class PlaceholderRuleTests(unittest.TestCase):
    """The notes replace only release.yml's placeholder, never notes someone wrote."""

    def make_tmp(self) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return Path(directory.name)

    def run_main(self, body: str) -> tuple[int, str, Path]:
        tmp = self.make_tmp()
        (tmp / "section.md").write_text(SECTION, encoding="utf-8")
        release = {"body": body, "assets": [{"name": name} for name in SUMS_ASSETS]}
        (tmp / "release.json").write_text(json.dumps(release), encoding="utf-8")
        output = tmp / "notes.md"
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            status = composer.main(
                ["--version", "0.10.0", "--template", str(TEMPLATE), "--changelog", str(tmp / "section.md"),
                 "--release-json", str(tmp / "release.json"), "--output", str(output)]
            )
        return status, stdout.getvalue() + stderr.getvalue(), output

    def test_the_placeholder_is_replaced(self):
        for body in (PLACEHOLDER_BODY, "\n  " + PLACEHOLDER_BODY):
            with self.subTest(body=body[:10]):
                status, out, output = self.run_main(body)
                self.assertEqual(status, 0, out)
                self.assertTrue(output.read_text(encoding="utf-8").startswith("# TaskSmack v0.10.0\n"))

    def test_hand_written_notes_are_left_as_they_are(self):
        for body in ("# TaskSmack v0.10.0\n\nHand-written.\n\n" + VERIFY, "Notes. Release notes follow.", ""):
            with self.subTest(body=body[:20]):
                status, out, output = self.run_main(body)
                self.assertEqual(status, 0, out)
                self.assertIn("release notes already set; left as is", out)
                self.assertFalse(output.exists())

    def test_bad_input_fails_without_writing(self):
        tmp = self.make_tmp()
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            status = composer.main(
                ["--version", "0.10.0", "--template", str(TEMPLATE), "--changelog", str(tmp / "missing.md"),
                 "--release-json", str(tmp / "missing.json"), "--output", str(tmp / "notes.md")]
            )
        self.assertEqual(status, 2)
        self.assertFalse((tmp / "notes.md").exists())


if __name__ == "__main__":
    unittest.main()
