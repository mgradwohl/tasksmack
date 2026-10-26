#!/usr/bin/env python3
"""Browser smoke test of the Docsify site under docs/ (#2967).

tools/md-link-audit.sh checks that every link's target file exists, resolving it from the Markdown
file. Docsify resolves links in the browser, from the current route, so a link the audit passes can
still land on "Page Not Found" or a missing image (#2953). This serves docs/ locally under the
production /tasksmack/ path, drives the real site in headless Chromium, and checks what a visitor
sees:

  - the root URL shows the User Guide, and its screenshots load;
  - a FAQ link from the User Guide, followed as a visitor clicks it, reaches the FAQ section;
  - the sidebar reaches the intended page from more than one nested page;
  - the download link points at the latest release, which exists (not downloaded);
  - every section link on the sidebar's pages reaches a heading on its page;
  - an unknown route shows the custom 404 page, and its User Guide link recovers;
  - no JavaScript error, and no first-party request that fails.

Usage: tools/docs-site-smoke.py [--docs DIR] [--offline] [--headed]
Needs Playwright and its Chromium, hash-pinned in requirements-docs-site.txt:
  python -m pip install --require-hashes -r requirements-docs-site.txt
  python -m playwright install --with-deps chromium   # or: install chromium, when the deps are there
"""

from __future__ import annotations

import argparse
import functools
import http.server
import re
import sys
import tempfile
import threading
from collections.abc import Callable
from pathlib import Path

from playwright.sync_api import Page, Request, Response, sync_playwright

REPO_ROOT = Path(__file__).resolve().parent.parent
SITE_PATH = "/tasksmack/"  # GitHub Pages serves the site from here
LATEST_RELEASE = "https://github.com/mgradwohl/tasksmack/releases/latest"
TIMEOUT_MS = 15_000

# Requests Docsify makes that may 404 by design: loadSidebar looks for a _sidebar.md beside the page
# before the root one, and the 404 check asks for a page that doesn't exist.
EXPECTED_404 = re.compile(r"/_sidebar\.md$|/no-such-page[^/]*\.md$")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:  # noqa: A002 - the base class's name
        pass


def serve(docs: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    """Serves @p docs at SITE_PATH on a free localhost port; returns the server and the site URL."""
    root = Path(tempfile.mkdtemp(prefix="docs-site-"))
    (root / SITE_PATH.strip("/")).symlink_to(docs.resolve(), target_is_directory=True)
    handler = functools.partial(QuietHandler, directory=str(root))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}{SITE_PATH}"


class Watch:
    """JavaScript errors and failed first-party requests, since the last take()."""

    def __init__(self, page: Page, site: str) -> None:
        self.site = site
        self.problems: list[str] = []
        page.on("pageerror", lambda error: self.problems.append(f"JavaScript error: {error}"))
        page.on("requestfailed", self.on_failed)
        page.on("response", self.on_response)

    def first_party(self, url: str) -> bool:
        return url.startswith(self.site)

    def on_failed(self, request: Request) -> None:
        if self.first_party(request.url):
            self.problems.append(f"request failed: {request.url} ({request.failure})")

    def on_response(self, response: Response) -> None:
        if self.first_party(response.url) and response.status >= 400 and not EXPECTED_404.search(response.url):
            self.problems.append(f"HTTP {response.status}: {response.url}")

    def take(self) -> list[str]:
        problems, self.problems = self.problems, []
        return problems


def content(page: Page):
    return page.locator(".markdown-section")


def expect_page(page: Page, heading: str) -> None:
    """Waits for the page whose first heading is @p heading; fails on Docsify's 404 page."""
    first = content(page).locator("h1").first
    first.wait_for(timeout=TIMEOUT_MS)
    page.wait_for_function(
        "h => document.querySelector('.markdown-section h1')?.textContent.trim() === h",
        arg=heading,
        timeout=TIMEOUT_MS,
    )


def expect_images_load(page: Page) -> None:
    images = content(page).locator("img")
    count = images.count()
    if count == 0:
        raise AssertionError("no images rendered")
    page.wait_for_function(
        "() => [...document.querySelectorAll('.markdown-section img')].every(i => i.complete)", timeout=TIMEOUT_MS
    )
    broken = page.evaluate(
        "() => [...document.querySelectorAll('.markdown-section img')]"
        ".filter(i => i.naturalWidth === 0).map(i => i.getAttribute('src'))"
    )
    if broken:
        raise AssertionError(f"images that didn't load: {broken}")


def click_link(page: Page, text: str) -> None:
    content(page).get_by_role("link", name=text, exact=True).first.click()


def click_sidebar(page: Page, text: str) -> None:
    """Clicks the first sidebar link named @p text: the User Guide's Overview comes before the Developer Guide's."""
    page.locator(".sidebar-nav").get_by_role("link", name=text, exact=True).first.click()


def check_home(page: Page, site: str, offline: bool) -> None:
    page.goto(site)
    expect_page(page, "User Guide")
    expect_images_load(page)


def check_faq_link(page: Page, site: str, offline: bool) -> None:
    page.goto(site)
    expect_page(page, "User Guide")
    click_link(page, "Acting on another user's process")
    expect_page(page, "FAQ & Troubleshooting")
    section = page.locator("#process-io-fds-or-network-show-na-on-linux")
    section.wait_for(state="attached", timeout=TIMEOUT_MS)
    page.wait_for_function(
        "() => { const r = document.getElementById('process-io-fds-or-network-show-na-on-linux')"
        ".getBoundingClientRect(); return r.top > -2 && r.top < innerHeight / 2; }",
        timeout=TIMEOUT_MS,
    )  # scrolled to the section, not left at the top of the page
    if "faq" not in page.url or "process-io-fds-or-network-show-na-on-linux" not in page.url:
        raise AssertionError(f"unexpected FAQ URL: {page.url}")


def check_sidebar(page: Page, site: str, offline: bool) -> None:
    page.goto(site + "#/dev/developer-guide")
    expect_page(page, "Developer Guide")
    click_sidebar(page, "FAQ & Troubleshooting")
    expect_page(page, "FAQ & Troubleshooting")
    click_sidebar(page, "Wayland Test Matrix")
    expect_page(page, "Wayland Native Support - Test Matrix and Validation")
    click_sidebar(page, "Overview")
    expect_page(page, "User Guide")


def check_download_link(page: Page, site: str, offline: bool) -> None:
    page.goto(site)
    expect_page(page, "User Guide")
    href = content(page).get_by_role("link", name="→ Latest Release").first.get_attribute("href")
    if href != LATEST_RELEASE:
        raise AssertionError(f"download link points at {href!r}, not {LATEST_RELEASE}")
    if offline:
        return
    # The release page only: no package is downloaded.
    response = page.request.get(href, max_redirects=5, timeout=TIMEOUT_MS)
    if not response.ok or "/releases/tag/" not in response.url:
        raise AssertionError(f"latest release didn't resolve: HTTP {response.status} at {response.url}")


# Every page the sidebar lists: check_anchors() follows their section links.
SIDEBAR_PAGES = [
    "guide/user-guide",
    "guide/faq",
    "code-signing-policy",
    "dev/developer-guide",
    "dev/completed-features",
    "dev/wayland-test-matrix",
]


def check_anchors(page: Page, site: str, offline: bool) -> None:
    """Every section link on the sidebar's pages reaches a heading Docsify made: its slugs differ from
    GitHub's for some characters (a heading's double quotes become "quot"), so faq.md#x can be a dead anchor."""
    targets: dict[str, set[str]] = {}
    for route in SIDEBAR_PAGES:
        page.goto(f"{site}#/{route}")
        content(page).locator("h1").first.wait_for(timeout=TIMEOUT_MS)
        hrefs = page.evaluate(
            "() => [...document.querySelectorAll('.markdown-section a[href*=\"?id=\"]')]"
            ".filter(a => !a.closest('h1,h2,h3,h4,h5,h6')).map(a => a.getAttribute('href'))"
        )
        for href in hrefs:
            match = re.fullmatch(r"#/([^?]*)\?id=(.+)", href)
            if match:
                targets.setdefault(match.group(1), set()).add(match.group(2))
    dead = []
    for route, ids in sorted(targets.items()):
        page.goto(f"{site}#/{route}")
        content(page).locator("h1").first.wait_for(timeout=TIMEOUT_MS)
        missing = page.evaluate("ids => ids.filter(id => !document.getElementById(decodeURIComponent(id)))", sorted(ids))
        dead += [f"#/{route}?id={anchor}" for anchor in missing]
    if dead:
        raise AssertionError(f"section links with no such heading: {dead}")


def check_not_found(page: Page, site: str, offline: bool) -> None:
    page.goto(site + "#/no-such-page")
    expect_page(page, "Page Not Found")
    click_link(page, "User Guide")
    expect_page(page, "User Guide")


CHECKS: list[tuple[str, Callable[[Page, str, bool], None]]] = [
    ("the root URL shows the User Guide and its screenshots", check_home),
    ("a FAQ link from the User Guide reaches its section", check_faq_link),
    ("the sidebar reaches pages from nested pages", check_sidebar),
    ("the download link points at the latest release", check_download_link),
    ("every section link on the sidebar's pages reaches its heading", check_anchors),
    ("an unknown route shows the 404 page, which recovers", check_not_found),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--docs", type=Path, default=REPO_ROOT / "docs", help="the site to test (default: docs/)")
    parser.add_argument("--offline", action="store_true", help="don't fetch the latest release page from GitHub")
    parser.add_argument("--headed", action="store_true", help="show the browser")
    args = parser.parse_args()

    server, site = serve(args.docs)
    failures = 0
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=not args.headed)
            for name, check in CHECKS:
                page = browser.new_page()
                page.set_default_timeout(TIMEOUT_MS)
                watch = Watch(page, site)
                try:
                    check(page, site, args.offline)
                    page.wait_for_load_state("networkidle", timeout=TIMEOUT_MS)
                    problems = watch.take()
                except Exception as error:  # noqa: BLE001 - every failure is reported, then the next check runs
                    message = str(error).splitlines()[0] if str(error) else ""
                    problems = [f"{type(error).__name__}: {message}", *watch.take()]
                if problems:
                    failures += 1
                    print(f"FAIL  {name} (at {page.url})")
                    for problem in problems:
                        print(f"      {problem}")
                else:
                    print(f"ok    {name}")
                page.close()
            browser.close()
    finally:
        server.shutdown()
    print(f"{len(CHECKS) - failures}/{len(CHECKS)} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
