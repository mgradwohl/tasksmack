#!/usr/bin/env python3
"""Tests for tools/docs-site-smoke.py's local server (#4528): it serves the docs folder at /tasksmack/, as GitHub
Pages does, without the symlink that needed a privilege on Windows. Playwright isn't needed for this part, so a stub
stands in for it."""

import importlib.util
import sys
import tempfile
import types
import unittest
import urllib.error
import urllib.request
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"


def load_smoke():
    stub = types.ModuleType("playwright.sync_api")
    for name in ("Page", "Request", "Response", "Route", "sync_playwright"):
        setattr(stub, name, object)
    sys.modules.setdefault("playwright", types.ModuleType("playwright"))
    sys.modules.setdefault("playwright.sync_api", stub)
    spec = importlib.util.spec_from_file_location("docs_site_smoke", TOOLS / "docs-site-smoke.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DocsSiteServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.smoke = load_smoke()

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        docs = Path(self._tmp.name) / "docs"
        (docs / "guide").mkdir(parents=True)
        # Bytes, not write_text: on Windows text mode would write "\r\n", and the test compares what is served.
        (docs / "index.html").write_bytes(b"<p>home</p>")
        (docs / "guide" / "user-guide.md").write_bytes(b"# User Guide\n")
        self.server, self.site = self.smoke.serve(docs)
        self.origin = self.site[: -len(self.smoke.SITE_PATH)]

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self._tmp.cleanup()

    def get(self, path: str) -> tuple[int, str]:
        try:
            with urllib.request.urlopen(self.origin + path, timeout=10) as response:
                return response.status, response.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            return error.code, ""

    def test_a_file_under_the_site_path_is_served(self) -> None:
        self.assertEqual(self.get("/tasksmack/guide/user-guide.md"), (200, "# User Guide\n"))
        self.assertEqual(self.get("/tasksmack/guide/user-guide.md?x=1"), (200, "# User Guide\n"))

    def test_the_site_root_serves_its_index(self) -> None:
        self.assertEqual(self.get("/tasksmack/"), (200, "<p>home</p>"))
        self.assertEqual(self.get("/tasksmack")[0], 200, "redirected to /tasksmack/, which the client follows")

    def test_anything_outside_the_site_path_is_not_found(self) -> None:
        self.assertEqual(self.get("/guide/user-guide.md")[0], 404)
        self.assertEqual(self.get("/")[0], 404)
        self.assertEqual(self.get("/tasksmackx/index.html")[0], 404)

    def test_the_site_url_is_the_github_pages_path(self) -> None:
        self.assertTrue(self.site.endswith("/tasksmack/"))


if __name__ == "__main__":
    unittest.main()
