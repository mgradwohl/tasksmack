#!/usr/bin/env python3
"""Tests for tools/docs-sri.py (#3167): the docs site's jsDelivr pins carry SRI hashes that match the
lockfile, a version bump fails the offline check until the hashes are regenerated, and --update writes
both the lockfile and the page's attributes. Never touches the network: --update gets a fake download.
"""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "docs-sri.py"
spec = importlib.util.spec_from_file_location("docs_sri", SCRIPT)
sri = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sri)

LIB = "https://cdn.jsdelivr.net/npm/lib@1.0.0/lib.min.js"
CSS = "https://cdn.jsdelivr.net/npm/lib@1.0.0/theme.css"
PAGE = f"""<html><head>
  <link
    rel="stylesheet"
    href="{CSS}"
  />
</head><body>
  <script>inline();</script>
  <script src="{LIB}"></script>
  <script src="local.js"></script>
</body></html>
"""


def fake_download(url: str) -> bytes:
    return f"contents of {url}".encode()


class DocsSriTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "docs").mkdir()
        (self.root / "docs" / "index.html").write_text(PAGE, encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def page(self) -> str:
        return (self.root / "docs" / "index.html").read_text(encoding="utf-8")

    def test_a_page_without_hashes_fails_the_check(self):
        problems = sri.check(self.root)
        self.assertTrue(any("no integrity attribute" in p for p in problems), problems)
        self.assertTrue(any("crossorigin" in p for p in problems), problems)

    def test_update_writes_the_lockfile_and_the_attributes_then_the_check_passes(self):
        sri.update(self.root, download=fake_download)
        lock = json.loads((self.root / "docs" / "cdn-integrity.json").read_text(encoding="utf-8"))
        self.assertEqual(set(lock), {LIB, CSS})
        self.assertEqual(lock[LIB], sri.sri(fake_download(LIB)))
        page = self.page()
        self.assertIn(f'<script src="{LIB}" integrity="{lock[LIB]}" crossorigin="anonymous"></script>', page)
        self.assertIn(f'    integrity="{lock[CSS]}"\n    crossorigin="anonymous"\n  />', page)  # the link keeps its layout
        self.assertIn('<script src="local.js"></script>', page)  # not a CDN pin: untouched
        self.assertIn("<script>inline();</script>", page)
        self.assertEqual(sri.check(self.root), [])
        # Idempotent: a second update changes nothing.
        before = self.page()
        sri.update(self.root, download=fake_download)
        self.assertEqual(self.page(), before)

    def test_a_version_bump_fails_until_regenerated(self):
        sri.update(self.root, download=fake_download)
        page = self.root / "docs" / "index.html"
        page.write_text(self.page().replace("lib@1.0.0/lib.min.js", "lib@1.1.0/lib.min.js"), encoding="utf-8")
        problems = sri.check(self.root)
        self.assertTrue(any("lib@1.1.0" in p and "not in" in p for p in problems), problems)
        self.assertTrue(any("lib@1.0.0/lib.min.js" in p and "no longer on the page" in p for p in problems), problems)
        sri.update(self.root, download=fake_download)
        self.assertEqual(sri.check(self.root), [])

    def test_a_tampered_hash_fails_the_check(self):
        sri.update(self.root, download=fake_download)
        page = self.root / "docs" / "index.html"
        page.write_text(self.page().replace("sha384-", "sha384-X", 1), encoding="utf-8")
        self.assertTrue(any("differs" in p for p in sri.check(self.root)))

    def test_the_repository_page_matches_its_lockfile(self):
        self.assertEqual(sri.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
