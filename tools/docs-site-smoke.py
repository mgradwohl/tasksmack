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
  - the release dashboard under release/ (#4524) draws a made-up release, splits its open count by lane and
    lists the open issues by kind or by lane (#4585), switches between releases, shows a hostile issue title as
    text, and says so when its data can't be loaded;
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
import json
import re
import sys
import threading
from collections.abc import Callable
from pathlib import Path
from urllib.parse import unquote

from playwright.sync_api import Page, Request, Response, Route, sync_playwright

REPO_ROOT = Path(__file__).resolve().parent.parent
SITE_PATH = "/tasksmack/"  # GitHub Pages serves the site from here
LATEST_RELEASE = "https://github.com/mgradwohl/tasksmack/releases/latest"
# The release dashboard (#4524) is a page of its own under the site, not a Docsify route. A relative sidebar
# link, even one marked ':ignore', stays a route, and the search plugin fetches release/README.md for it, so
# the sidebar links the page by its full address.
RELEASE_DASHBOARD = "https://mgradwohl.github.io/tasksmack/release/"
TIMEOUT_MS = 15_000

# The release dashboard's data (#4524): tests/tools/test_release_dashboard.py builds a made-up repository and
# keeps these files equal to what tools/release-dashboard.py writes for it, so the page is tested against the
# generator's real schema. They stand in for the dashboard-data branch the page reads in production.
RELEASE_FIXTURES = REPO_ROOT / "tests" / "tools" / "fixtures" / "release-dashboard"
RELEASE_DATA = re.compile(r"^https://raw\.githubusercontent\.com/mgradwohl/tasksmack/dashboard-data/([\w.-]+\.json)$")

# Requests Docsify makes that may 404 by design: loadSidebar looks for a _sidebar.md beside the page
# before the root one, and the 404 check asks for a page that doesn't exist.
EXPECTED_404 = re.compile(r"/_sidebar\.md$|/no-such-page[^/]*\.md$")


class SiteHandler(http.server.SimpleHTTPRequestHandler):
    """Serves the docs folder at SITE_PATH, as GitHub Pages does, and answers 404 for anything outside it. No symlink
    to the folder (#4528): making one needs a privilege on Windows, so the test couldn't start in an ordinary shell."""

    def translate_path(self, path: str) -> str:
        bare = path.split("?", 1)[0].split("#", 1)[0]
        prefix = SITE_PATH.rstrip("/")
        if bare == prefix or bare.startswith(SITE_PATH):
            # "/tasksmack" maps to the folder itself, and the base class redirects it to "/tasksmack/".
            return super().translate_path(path[len(prefix) :] or "/")
        return str(Path(self.directory) / ".outside-the-site")  # a name no docs file has: 404

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002 - the base class's name
        pass


def serve(docs: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    """Serves @p docs at SITE_PATH on a free localhost port; returns the server and the site URL."""
    handler = functools.partial(SiteHandler, directory=str(docs.resolve()))
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


# Every column header on the release dashboard whose text isn't on the same edge as its column's figures, as
# "table | header | pixels off" (#4524: a two-row header's "24 h" took the first column's left alignment and sat
# beside its figures, not over them). A header that spans columns is skipped. Tables inside closed <details> are
# opened to be measured, then put back.
HEADERS_OFF_THEIR_COLUMNS = r"""
() => {
  const edges = (cell) => {
    const range = document.createRange();
    range.selectNodeContents(cell);
    const rects = [...range.getClientRects()].filter((rect) => rect.width > 0);
    return rects.length ? [Math.min(...rects.map((rect) => rect.left)), Math.max(...rects.map((rect) => rect.right))] : null;
  };
  const closed = [...document.querySelectorAll('details:not([open])')];
  closed.forEach((node) => { node.open = true; });
  const off = [];
  document.querySelectorAll('#app table').forEach((table, index) => {
    const rows = [...table.querySelectorAll('tbody tr')].filter((row) => !row.classList.contains('divider'));
    table.querySelectorAll('thead th').forEach((header) => {
      const head = edges(header);
      if (header.colSpan > 1 || !head) return;
      const box = header.getBoundingClientRect();
      const middle = (box.left + box.right) / 2;
      const right = getComputedStyle(header).textAlign === 'right';
      for (const row of rows) {
        const cell = [...row.children].find((child) => {
          const bounds = child.getBoundingClientRect();
          return bounds.left <= middle && middle <= bounds.right;
        });
        const body = cell && cell.textContent.trim() ? edges(cell) : null;
        if (!body) continue;
        const sameSide = (getComputedStyle(cell).textAlign === 'right') === right;
        const gap = sameSide ? Math.abs(right ? head[1] - body[1] : head[0] - body[0]) : Infinity;
        if (gap > 1.5) {
          off.push('table ' + index + ' | ' + header.textContent.trim() + ' | ' + (sameSide ? gap.toFixed(1) + 'px' : 'other side'));
          break;
        }
      }
    });
  });
  closed.forEach((node) => { node.open = false; });
  return off;
}
"""


def serve_release_data(page: Page, *, missing: bool = False) -> None:
    """Answers the dashboard's data requests from RELEASE_FIXTURES, or with 404 when @p missing."""

    def answer(route: Route) -> None:
        match = RELEASE_DATA.match(route.request.url)
        path = RELEASE_FIXTURES / match.group(1) if match else None
        if missing or path is None or not path.is_file():
            route.fulfill(status=404, body="", headers={"Access-Control-Allow-Origin": "*"})
        else:
            route.fulfill(
                status=200,
                content_type="application/json",
                body=path.read_text(encoding="utf-8"),
                headers={"Access-Control-Allow-Origin": "*"},
            )

    page.route("https://raw.githubusercontent.com/**", answer)


def expect_release(page: Page, title: str) -> None:
    page.wait_for_function("t => document.querySelector('#app h1')?.textContent === t", arg=title, timeout=TIMEOUT_MS)


def check_release_dashboard(page: Page, site: str, offline: bool) -> None:
    """docs/release/ draws the release in progress, from the generator's own output for a made-up repository."""
    errors: list[str] = []  # a refused inline script or style (the page's Content-Security-Policy) lands here

    def on_console(message) -> None:  # noqa: ANN001 - Playwright's ConsoleMessage
        if message.type == "error":
            errors.append(message.text)

    page.on("console", on_console)
    try:
        _check_release_dashboard(page, site)
    finally:
        page.remove_listener("console", on_console)  # a later check on this page reports its own errors
    if errors:
        raise AssertionError(f"console errors: {errors}")


def _check_release_dashboard(page: Page, site: str) -> None:
    serve_release_data(page)
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    current = json.loads((RELEASE_FIXTURES / f"{index['current']}.json").read_text(encoding="utf-8"))

    page.goto(site + "release/")
    expect_release(page, index["current"])
    rows = page.locator(".ship > *")
    if rows.count() != len(current["ship_list"]):
        raise AssertionError(f"{rows.count()} ship-list rows, not {len(current['ship_list'])}")
    stage = page.locator(".stages li[aria-current='step']").text_content() or ""
    if not stage.startswith("Feature freeze"):
        raise AssertionError(f"the current stage reads {stage!r}, not Feature freeze")
    # The milestone's due date is the goal: under "Released", beside the date it really shipped (not yet).
    released = page.locator(".stages li").last.text_content() or ""
    if "Goal: " not in released or "Actual: TBD" not in released:
        raise AssertionError(f"the Released stage reads {released!r}, with no goal or no 'Actual: TBD'")
    if page.locator(".chart svg path.line").count() != 2:
        raise AssertionError("the burn-up didn't draw its two lines")
    if page.locator(".chart svg path.area").count() != 3:
        raise AssertionError("the chart of the open work didn't draw its three bands")
    # The section's three charts share one time axis: the same day labels, at the same places.
    ticks = page.evaluate(
        "() => [...document.querySelectorAll('.chart svg')].map(chart => [...chart.querySelectorAll("
        "'text[text-anchor=middle]')].map(label => label.textContent + '@' + Math.round(label.getBoundingClientRect().left)))"
    )
    if len(ticks) != 3 or not ticks[0] or ticks[1] != ticks[0] or ticks[2] != ticks[0]:
        raise AssertionError(f"the three charts' day labels don't line up: {ticks}")
    # Every column header sits over its column's figures.
    off = page.evaluate(HEADERS_OFF_THEIR_COLUMNS)
    if off:
        raise AssertionError(f"column headers off their columns: {off}")
    if page.locator(".hero-figure").text_content() != str(current["counts"]["open"]):
        raise AssertionError("the open-issue count isn't the fixture's")
    _check_release_summary(page, current)
    _check_release_lists(page, current)

    # An issue title is someone else's text: it's shown as written, and never becomes markup.
    hostile = next((issue["title"] for issue in current["open_issues"] if "<img" in issue["title"]), None)
    if hostile is None:
        raise AssertionError("the fixtures hold no issue title with markup in it: tests/tools/test_release_dashboard.py")
    shown = page.evaluate("() => [...document.querySelectorAll('.item-title')].map(node => node.textContent)")
    if not any(hostile in text for text in shown):
        raise AssertionError("the hostile issue title isn't shown as text")
    injected = page.evaluate("() => [document.querySelectorAll('#app img, #app b').length, window.__xss ?? null]")
    if injected != [0, None]:
        raise AssertionError(f"an issue title became markup: {injected}")

    # A row opens to its issues, and the links are built from the numbers.
    page.locator("details[data-key='ship-severity'] > summary").click()
    href = page.locator("details[data-key='ship-severity'] .item-number").first.get_attribute("href")
    if href != "https://github.com/mgradwohl/tasksmack/issues/102":
        raise AssertionError(f"the first severity issue links to {href!r}")

    # The comparison table judges this release against the one before it.
    if page.locator(".compare td.better, .compare td.worse").count() == 0:
        raise AssertionError("the comparison table shows no change against the previous release")

    # The picker switches release, and ?m= opens one directly. A planned release has only its scope.
    page.select_option("#release-select", "v1.2.0")
    expect_release(page, "v1.2.0")
    if page.locator(".ship > *").count() != 1:
        raise AssertionError("the planned release shows more than its scope")
    page.goto(site + "release/?m=v1.0.0")
    expect_release(page, "v1.0.0")
    shipped = page.locator(".stages li").last.text_content() or ""
    if "Actual: " not in shipped or "TBD" in shipped:
        raise AssertionError(f"the shipped release's Released stage reads {shipped!r}, with no date")
    # An unknown ?m= falls back to the release in progress, and what it said is nowhere on the page.
    page.goto(site + "release/?m=v9.9.9%3Cscript%3E")
    expect_release(page, index["current"])
    echoed = page.evaluate("() => [document.body.innerHTML.includes('v9.9.9'), document.querySelectorAll('#app script').length]")
    if echoed != [False, 0]:
        raise AssertionError(f"the page echoed an unknown ?m= value: {echoed}")


# Where the summary's two panes stand at a desktop width (#4548): the top of the text of the line under the large
# figure, and of the pace table's caption beside it.
SUMMARY_TEXT_TOPS = r"""
() => {
  const top = (selector) => {
    const range = document.createRange();
    range.selectNodeContents(document.querySelector(selector));
    return Math.min(...[...range.getClientRects()].filter((rect) => rect.width > 0).map((rect) => rect.top));
  };
  return [top('.hero-label'), top('.summary caption')];
}
"""


def _check_release_summary(page: Page, current: dict) -> None:
    """The open count is split by lane (#4585), says what it leaves out, and the pace table starts under it (#4548)."""
    lanes = {entry["name"]: entry["open"] for entry in current["by_lane"]}
    shown = page.evaluate(
        "() => [...document.querySelectorAll('.lanes li')].map(lane => "
        "[lane.querySelector('.lane-name').textContent, Number(lane.querySelector('.lane-figure').textContent)])"
    )
    # The two lanes always, then any other group that has something open, under the page's name for it.
    others = {"Shared": "Both lanes", "Dash": "Dash", "None": "No lane"}
    expected = [[name, lanes.get(name, 0)] for name in ("Windows", "WSL")]
    expected += [[text, lanes[name]] for name, text in others.items() if lanes.get(name)]
    if len(expected) < 3:
        raise AssertionError("the fixtures hold no open issue with both lanes' labels: tests/tools/test_release_dashboard.py")
    if shown != expected:
        raise AssertionError(f"the open count's lanes read {shown}, not {expected}")
    if sum(count for _, count in shown) != current["counts"]["open"]:
        raise AssertionError(f"the lanes' figures {shown} don't add up to the {current['counts']['open']} open issues")
    # Each figure links to the search that lists the same issues on GitHub: a lane's own leaves the other lanes'
    # labels out, and "Both lanes" asks for any two, as the generator's lane() counts them.
    links = [unquote(href) for href in page.evaluate("() => [...document.querySelectorAll('.lanes a')].map(a => a.getAttribute('href'))")]
    search = f"/issues?q=is:issue is:open milestone:{current['milestone']['title']} "
    if len(links) < 3 or search + "label:agent:Windows -label:agent:WSL -label:agent:Dash" not in links[0]:
        raise AssertionError(f"the Windows lane's figure links to {links[:1]}")
    if search + "((label:agent:Windows AND label:agent:WSL) OR " not in links[2]:
        raise AssertionError(f"the Both lanes figure links to {links[2]!r}")
    # GitHub's milestone page counts the tracker and pull requests too: the page says why its own count is lower.
    note = page.locator(".hero .footnote").text_content() or ""
    if f"#{current['tracker']['number']}" not in note or "pull requests" not in note:
        raise AssertionError(f"the note under the open count reads {note!r}, without the tracker or pull requests")
    # The pace table's caption is level with the line under the large figure, not with the figure's top.
    label, caption = page.evaluate(SUMMARY_TEXT_TOPS)
    if abs(label - caption) > 3:
        raise AssertionError(f"the pace table's caption is {caption - label:+.1f}px off the line under the open count")


def _check_release_lists(page: Page, current: dict) -> None:
    """The open issues are listed by kind, or by lane at a click (#4585), and every issue is in one list."""

    def groups() -> list[list]:
        return page.evaluate(
            "() => [...document.querySelectorAll('.open-lists details')].map(group => "
            "[group.dataset.key, group.querySelectorAll('.items > li').length, group.open])"
        )

    total = len(current["open_issues"])
    kinds = groups()
    if not kinds or any(not key.startswith("open-") or key.startswith("open-lane-") for key, _, _ in kinds):
        raise AssertionError(f"the open issues aren't listed by kind to begin with: {kinds}")
    page.locator(".switch button[data-by='lane']").click()
    lanes = groups()
    if [key for key, _, _ in lanes] != ["open-lane-Windows", "open-lane-WSL", "open-lane-Shared"]:
        raise AssertionError(f"listed by lane, the groups are {lanes}")
    if sum(count for _, count, _ in lanes) != total or sum(count for _, count, _ in kinds) != total:
        raise AssertionError(f"the lists don't hold all {total} open issues: by kind {kinds}, by lane {lanes}")
    if not lanes[0][2] or page.locator(".switch button[data-by='lane']").get_attribute("aria-pressed") != "true":
        raise AssertionError("after the switch, the first lane's list isn't open or the Lane button isn't pressed")
    # A lane's list runs in docs/dev/labels.md's queue order: the steps in turn (security and bugs are one), then
    # the highest priority, then the most severe, then the oldest.
    steps = ["CI", "Tooling", "Bug", "Perf", "Testing", "Docs", "Refactor", "Parity", "Feature", "Other"]
    levels = {"high": 0, "medium": 1, "low": 2}

    def queue(issue: dict) -> tuple[int, int, int, int]:
        kind = "Bug" if issue["category"] == "Security" else issue["category"]
        return (steps.index(kind), levels.get(issue["pri"], 3), levels.get(issue["sev"], 3), issue["n"])

    for lane in ("Windows", "WSL", "Shared"):
        expected = [f"#{issue['n']}" for issue in sorted((issue for issue in current["open_issues"] if issue["lane"] == lane), key=queue)]
        listed = page.evaluate(
            "key => [...document.querySelectorAll(`details[data-key='${key}'] .item-number`)].map(node => node.textContent)",
            f"open-lane-{lane}",
        )
        if listed != expected:
            raise AssertionError(f"the {lane} list runs {listed}, not in queue order {expected}")
    # Under "Both lanes" an issue's chip says so, not the data's word for it.
    chips = page.evaluate("() => [...document.querySelectorAll(\"details[data-key='open-lane-Shared'] .chip\")].map(node => node.textContent)")
    if "Both lanes" not in chips or "Shared" in chips:
        raise AssertionError(f"the Both lanes list's chips read {chips}")
    # Back to kinds: the rest of this check, and the next visit, start from the page's default.
    page.locator(".switch button[data-by='kind']").click()
    if groups()[0][0] != kinds[0][0]:
        raise AssertionError("the switch didn't go back to listing by kind")


def check_release_dashboard_without_data(page: Page, site: str, offline: bool) -> None:
    """With no data to read (no branch yet, or GitHub is down), the page says so and links the milestones."""
    serve_release_data(page, missing=True)
    page.goto(site + "release/")
    notice = page.locator("#app .notice")
    notice.get_by_role("link", name="The milestones page").wait_for(timeout=TIMEOUT_MS)
    if "couldn't be loaded" not in (notice.text_content() or ""):
        raise AssertionError(f"unexpected notice: {notice.text_content()!r}")


def check_release_dashboard_link(page: Page, site: str, offline: bool) -> None:
    """The sidebar's Release dashboard link is the page's full address, which Docsify leaves alone."""
    page.goto(site + "#/dev/developer-guide")
    expect_page(page, "Developer Guide")
    href = page.locator(".sidebar-nav").get_by_role("link", name="Release dashboard", exact=True).get_attribute("href")
    if href != RELEASE_DASHBOARD:
        raise AssertionError(f"the sidebar's Release dashboard link points at {href!r}, not {RELEASE_DASHBOARD}")
    if not (REPO_ROOT / "docs" / "release" / "index.html").is_file():
        raise AssertionError("docs/release/index.html, the page that address serves, is missing")


CHECKS: list[tuple[str, Callable[[Page, str, bool], None]]] = [
    ("the root URL shows the User Guide and its screenshots", check_home),
    ("a FAQ link from the User Guide reaches its section", check_faq_link),
    ("the sidebar reaches pages from nested pages", check_sidebar),
    ("the download link points at the latest release", check_download_link),
    ("every section link on the sidebar's pages reaches its heading", check_anchors),
    ("an unknown route shows the 404 page, which recovers", check_not_found),
    ("the release dashboard draws a release and switches between them", check_release_dashboard),
    ("the release dashboard says when its data can't be loaded", check_release_dashboard_without_data),
    ("the sidebar links the release dashboard", check_release_dashboard_link),
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
