#!/usr/bin/env python3
"""Tests for tools/md-link-audit.sh and its Windows twin tools/md-link-audit.ps1, the docs-hygiene link check.

- Code isn't scanned for links (#3001): an inline code span (any number of backticks) or a fenced block
  holding "[a][b]" is code, not a reference link, while a real "[text][missing]" is still reported.
- Under docs/, a root-absolute link resolves from the docsify site root, docs/ (#2953), in an inline
  link and in a reference definition; elsewhere it resolves from the repository root. The .ps1 resolved
  both against the repository root (#3529).

The script runs against a scratch tree passed as its repository root. MdLinkAuditTest runs the .sh and
needs bash. MdLinkAuditPs1Test runs the .ps1 with the PowerShell 7 in TASKSMACK_TEST_PWSH, and skips
without it; it also checks the checkout itself has no broken link, as CI's .sh run does.
"""

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"
PWSH = os.environ.get("TASKSMACK_TEST_PWSH")


class MdLinkAuditTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="md_link_audit_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def command(self, root):
        return [BASH, str(REPO_ROOT / "tools" / "md-link-audit.sh"), str(root)]

    def audit(self, root=None):
        return subprocess.run(
            self.command(root or self.root),
            capture_output=True,
            text=True,
            check=False,
        )

    def test_code_spans_and_fenced_blocks_are_not_links_but_a_real_missing_reference_is(self):
        (self.root / "README.md").write_text(
            textwrap.dedent(
                """\
                # Title

                A pattern `[AE]OC CA [0-9][0-9]` and ``[c][d] with a ` inside`` are code.

                ```
                [e][f]
                ```

                ~~~text
                [g][h]
                ~~~

                But [text][missing] is a link.
                """
            ),
            encoding="utf-8",
        )
        result = self.audit()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Broken internal link findings: 1", result.stdout)
        self.assertIn("Missing reference definition for [missing]", result.stdout)
        for code in ("[0-9]", "[d]", "[f]", "[h]"):
            self.assertNotIn(f"definition for {code}", result.stdout)

    def test_a_root_absolute_link_under_docs_resolves_from_docs(self):
        (self.root / "docs" / "guide").mkdir(parents=True)
        (self.root / "docs" / "guide" / "page.md").write_text("# Page\n\n## Section\n", encoding="utf-8")
        (self.root / "docs" / "_sidebar.md").write_text(
            textwrap.dedent(
                """\
                - [Page](/guide/page.md)
                - [Section](/guide/page.md#section)
                - [By reference][page]

                [page]: /guide/page.md#section
                """
            ),
            encoding="utf-8",
        )
        # Outside docs/, the same kind of link resolves from the repository root.
        (self.root / "README.md").write_text("[Guide](/docs/guide/page.md)\n", encoding="utf-8")
        result = self.audit()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Broken internal link findings: 0", result.stdout)

    def test_a_missing_root_absolute_target_under_docs_is_reported_against_docs(self):
        (self.root / "docs").mkdir()
        (self.root / "docs" / "_sidebar.md").write_text("- [Gone](/guide/gone.md)\n", encoding="utf-8")
        # It exists at the repository root, so only a lookup from docs/ reports it.
        (self.root / "guide").mkdir()
        (self.root / "guide" / "gone.md").write_text("# Gone\n", encoding="utf-8")
        result = self.audit()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Broken internal link findings: 1", result.stdout)
        self.assertIn("docs/_sidebar.md:1: /guide/gone.md", result.stdout)


@unittest.skipUnless(PWSH, "TASKSMACK_TEST_PWSH not set")
class MdLinkAuditPs1Test(MdLinkAuditTest):
    def command(self, root):
        return [PWSH, "-NoLogo", "-NoProfile", "-File", str(REPO_ROOT / "tools" / "md-link-audit.ps1"), "-RepoRoot", str(root)]

    def test_the_checkout_has_no_broken_link(self):
        result = self.audit(REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Broken internal link findings: 0", result.stdout)


if __name__ == "__main__":
    unittest.main()
