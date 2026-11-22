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
    lists the open issues by kind or by lane (#4585), ends a lane's list with its own group for the issues that
    wait on the maintainer (#4718), switches between releases, shows a hostile issue title as text, and says so
    when its data can't be loaded;
  - the release dashboard flags data over 25 minutes old, and its "Refresh now" link opens the workflow on GitHub
    and has the page look for new data every 30 seconds (#4604);
  - the release dashboard says when GitHub's API budget is used up and when it returns, in place of the
    stale banner, until that time passes or new data arrives (#4726);
  - a late release dashboard says why, from GitHub's API: nothing when newer figures are published and only
    the page's copy is old, and otherwise, from the refresh workflow's runs, a refresh waiting for a runner,
    one running, the last one failed, or none published; and nothing when the API can't be read (#4734,
    #4741). No check reaches the real API;
  - no JavaScript error, and no first-party request that fails.

Usage: tools/docs-site-smoke.py [--docs DIR] [--offline] [--headed]
Needs Playwright and its Chromium, hash-pinned in requirements-docs-site.txt:
  python -m pip install --require-hashes -r requirements-docs-site.txt
  python -m playwright install --with-deps chromium   # or: install chromium, when the deps are there
"""

from __future__ import annotations

import argparse
import datetime
import functools
import http.server
import json
import re
import sys
import threading
import time
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


def serve_release_data(page: Page, *, missing: bool = False, change: Callable[[str, dict], None] | None = None) -> None:
    """Answers the dashboard's data requests from RELEASE_FIXTURES, or with 404 when @p missing. @p change may
    alter a file's data before it is served: it gets the file's name and what the file holds."""

    def answer(route: Route) -> None:
        match = RELEASE_DATA.match(route.request.url)
        path = RELEASE_FIXTURES / match.group(1) if match else None
        if missing or path is None or not path.is_file():
            route.fulfill(status=404, body="", headers={"Access-Control-Allow-Origin": "*"})
            return
        body = path.read_text(encoding="utf-8")
        if change is not None:
            data = json.loads(body)
            change(path.name, data)
            body = json.dumps(data)
        route.fulfill(status=200, content_type="application/json", body=body, headers={"Access-Control-Allow-Origin": "*"})

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

    # A lane can't take an issue the ship list counts as waiting on the maintainer, so those come after the
    # lane's own work, in a group of their own (#4718). Each part runs in queue order.
    held = next(row["issues"] for row in current["ship_list"] if row["id"] == "maintainer")
    if not held:
        raise AssertionError("the fixtures hold no issue waiting on the maintainer: tests/tools/test_release_dashboard.py")
    with_waiting = 0
    for lane in ("Windows", "WSL", "Shared"):
        in_queue = sorted((issue for issue in current["open_issues"] if issue["lane"] == lane), key=queue)
        own = [f"#{issue['n']}" for issue in in_queue if issue["n"] not in held]
        waiting = [f"#{issue['n']}" for issue in in_queue if issue["n"] in held]
        listed = lane_list(page, lane)
        if listed["all"] != own + waiting:
            raise AssertionError(f"the {lane} list runs {listed['all']}, not its own work {own} and then what waits on the maintainer {waiting}")
        if listed["held"] != waiting:
            raise AssertionError(f"the {lane} list's waiting group holds {listed['held']}, not {waiting}")
        said = f"{len(waiting)} waiting on the maintainer"
        if waiting:
            with_waiting += 1
            if listed["heading"] != ["Waiting on the maintainer", f"{len(waiting)} issue{'' if len(waiting) == 1 else 's'}"]:
                raise AssertionError(f"the {lane} list's waiting group is headed {listed['heading']}")
            if listed["heading_text"] != f"Waiting on the maintainer {len(waiting)} issue{'' if len(waiting) == 1 else 's'}":
                raise AssertionError(f"the {lane} list's waiting group reads {listed['heading_text']!r} to a screen reader")
            started = sum(1 for issue in in_queue if issue["n"] not in held and issue["phase"] != "todo")
            summary = f"{len(in_queue)} issue{'' if len(in_queue) == 1 else 's'}" + (f", {started} started" if started else "") + f", {said}"
            if listed["summary"] != summary:
                raise AssertionError(f"the {lane} list's summary reads {listed['summary']!r}, not {summary!r}")
            # The heading says it once: the rows under it don't each say it again.
            if "Waiting on the maintainer" in listed["held_chips"]:
                raise AssertionError(f"the {lane} list's waiting rows repeat their heading: {listed['held_chips']}")
        elif listed["heading"] or "waiting" in listed["summary"]:
            raise AssertionError(f"the {lane} list has nothing waiting, and shows {listed['heading']} and {listed['summary']!r}")
    if with_waiting in (0, 3):
        raise AssertionError("the fixtures need a lane with an issue waiting on the maintainer and a lane with none")
    # Under "Both lanes" an issue's chip says so, not the data's word for it.
    chips = page.evaluate("() => [...document.querySelectorAll(\"details[data-key='open-lane-Shared'] .chip\")].map(node => node.textContent)")
    if "Both lanes" not in chips or "Shared" in chips:
        raise AssertionError(f"the Both lanes list's chips read {chips}")
    # Back to kinds: the rest of this check, and the next visit, start from the page's default.
    page.locator(".switch button[data-by='kind']").click()
    if groups()[0][0] != kinds[0][0]:
        raise AssertionError("the switch didn't go back to listing by kind")
    # Listed by kind there is no such group, so each waiting issue says so on its own row.
    marked = page.evaluate(
        "() => [...document.querySelectorAll('.open-lists .items > li')].filter(row => [...row.querySelectorAll('.chip')]"
        ".some(chip => chip.textContent === 'Waiting on the maintainer')).map(row => row.querySelector('.item-number').textContent)"
    )
    if sorted(marked) != sorted(f"#{number}" for number in held):
        raise AssertionError(f"listed by kind, the issues marked as waiting on the maintainer are {marked}, not {held}")
    if page.locator(".open-lists .held").count():
        raise AssertionError("listed by kind, a list has a waiting-on-the-maintainer group")


def lane_list(page: Page, lane: str) -> dict:
    """What a lane's list shows when the open issues are listed by lane: its issue numbers in order, the ones in
    its waiting-on-the-maintainer group, that group's heading and chips, and the summary's count."""
    return page.evaluate(
        "key => { const group = document.querySelector(`details[data-key='${key}']`);"
        " const texts = nodes => [...nodes].map(node => node.textContent);"
        " return { all: texts(group.querySelectorAll('.item-number')),"
        " own: texts(group.querySelectorAll(':scope > .items .item-number')),"
        " held: texts(group.querySelectorAll('.held .item-number')),"
        " heading: texts(group.querySelectorAll('.held > h4 span')),"
        " heading_text: (group.querySelector('.held > h4') || {}).textContent || '',"
        " held_chips: texts(group.querySelectorAll('.held .chip')),"
        " summary: group.querySelector('summary > span:last-child').textContent }; }",
        f"open-lane-{lane}",
    )


def check_release_dashboard_lane_all_waiting(page: Page, site: str, offline: bool) -> None:
    """A lane whose every open issue waits on the maintainer shows that group alone, and says so in its summary (#4718)."""
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    current = json.loads((RELEASE_FIXTURES / f"{index['current']}.json").read_text(encoding="utf-8"))
    windows = sorted(issue["n"] for issue in current["open_issues"] if issue["lane"] == "Windows")
    if len(windows) < 2:
        raise AssertionError("the fixtures need two open Windows issues: tests/tools/test_release_dashboard.py")

    def hold_the_lane(name: str, data: dict) -> None:
        for row in data.get("ship_list", []):
            if row["id"] == "maintainer":
                row["issues"] = sorted(set(row["issues"]) | set(windows))

    serve_release_data(page, change=hold_the_lane)
    page.goto(site + "release/")
    expect_release(page, index["current"])
    page.locator(".switch button[data-by='lane']").click()
    listed = lane_list(page, "Windows")
    if listed["own"] or sorted(int(text.lstrip("#")) for text in listed["held"]) != windows:
        raise AssertionError(f"with every Windows issue waiting, the list shows its own work {listed['own']} and waiting {listed['held']}")
    if page.locator("details[data-key='open-lane-Windows'] > .items").count():
        raise AssertionError("with every Windows issue waiting, the list still draws an empty list of the lane's own work")
    # No "started" here, whatever labels the issues carry: that count is the lane's own work, and it has none.
    expected = f"{len(windows)} issues, {len(windows)} waiting on the maintainer"
    if listed["summary"] != expected:
        raise AssertionError(f"the Windows list's summary reads {listed['summary']!r}, not {expected!r}")
    untouched = lane_list(page, "WSL")
    if untouched["held"] or untouched["heading"]:
        raise AssertionError(f"the WSL list has nothing waiting, and shows {untouched['held']}")


def check_release_dashboard_without_data(page: Page, site: str, offline: bool) -> None:
    """With no data to read (no branch yet, or GitHub is down), the page says so and links the milestones."""
    serve_release_data(page, missing=True)
    page.goto(site + "release/")
    notice = page.locator("#app .notice")
    notice.get_by_role("link", name="The milestones page").wait_for(timeout=TIMEOUT_MS)
    if "couldn't be loaded" not in (notice.text_content() or ""):
        raise AssertionError(f"unexpected notice: {notice.text_content()!r}")


# The dashboard's "Refresh now" link (#4604): the workflow's page on GitHub, where "Run workflow" starts a run.
RELEASE_WORKFLOW = "https://github.com/mgradwohl/tasksmack/actions/workflows/release-dashboard.yml"


def check_release_dashboard_refresh(page: Page, site: str, offline: bool) -> None:
    """Data over 25 minutes old is flagged, and "Refresh now" has the page look for new data every 30 seconds (#4604)."""
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    current = index["current"]
    generated = datetime.datetime.fromisoformat(index["generated_at"].replace("Z", "+00:00"))
    # What a later run has published, as the data host serves it: a file's name, and the generated_at it then
    # carries. The host keeps each file for a few minutes by itself, so the index can be a run ahead of a release.
    newer: dict[str, str] = {}

    def answer(route: Route) -> None:
        match = RELEASE_DATA.match(route.request.url)
        path = RELEASE_FIXTURES / match.group(1) if match else None
        if path is None or not path.is_file():
            route.fulfill(status=404, body="", headers={"Access-Control-Allow-Origin": "*"})
            return
        data = json.loads(path.read_text(encoding="utf-8"))
        if path.name in newer:
            data["generated_at"] = newer[path.name]
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps(data),
            headers={"Access-Control-Allow-Origin": "*"},
        )

    page.route("https://raw.githubusercontent.com/**", answer)
    # The page's own clock (Date and its timers), set 24 minutes after the data was generated: under the limit.
    page.clock.install(time=generated + datetime.timedelta(minutes=24))
    page.goto(site + "release/")
    expect_release(page, current)
    banner = page.locator(".banner.stale")
    watching = page.locator(".watching")
    updated = page.locator(".updated > span:first-child")
    if banner.is_visible():
        raise AssertionError(f"data 24 minutes old is flagged: {banner.text_content()!r}")
    page.clock.fast_forward(2 * 60 * 1000)
    banner.wait_for(state="visible", timeout=TIMEOUT_MS)
    if "No new data for 26 min" not in (banner.text_content() or ""):
        raise AssertionError(f"the banner reads {banner.text_content()!r}, not how long ago the last run was")

    # Beside the age and in the banner: the workflow's page, in a tab of its own.
    found = [[link.get_attribute(name) for name in ("href", "target", "rel")] for link in page.locator("a.refresh").all()]
    if found != [[RELEASE_WORKFLOW, "_blank", "noopener"]] * 2:
        raise AssertionError(f"the 'Refresh now' links are {found}")

    # The click leaves for GitHub: that is stopped here, and the page's own handler still runs.
    if watching.is_visible():
        raise AssertionError("the page says it's looking for new data before 'Refresh now' is clicked")
    page.evaluate(
        "() => document.addEventListener('click', (event) => {"
        " if (event.target.closest('a.refresh')) event.preventDefault(); }, true)"
    )
    page.locator(".masthead a.refresh").click()
    watching.wait_for(state="visible", timeout=TIMEOUT_MS)

    # A run publishes 25 seconds later, and the host serves the new index before the new release file. The
    # page's next look is half a minute on, not its usual five minutes. It draws the pair it got and keeps
    # looking: the last run is recent, so the banner goes, and the release's figures are still the old ones.
    published = page.evaluate("() => new Date(Date.now() + 25000).toISOString()")
    newer["index.json"] = published
    page.evaluate("() => { document.querySelector('.masthead').dataset.before = '1'; }")
    page.clock.fast_forward(31 * 1000)
    page.wait_for_function("() => !document.querySelector('.masthead').dataset.before", timeout=TIMEOUT_MS)
    if banner.is_visible() or not watching.is_visible() or "min ago" not in (updated.text_content() or ""):
        raise AssertionError(
            "with a new index and the old release file, the page should drop the banner, keep the release's own "
            f"age and keep looking: banner {banner.is_visible()}, looking {watching.is_visible()}, "
            f"{updated.text_content()!r}"
        )

    # The release file follows. The next look, half a minute on again, has the whole run and stops looking.
    newer[f"{current}.json"] = published
    page.clock.fast_forward(31 * 1000)
    page.wait_for_function(
        "() => document.querySelector('.updated span')?.textContent === 'Updated just now'", timeout=TIMEOUT_MS
    )
    if banner.is_visible() or watching.is_visible():
        raise AssertionError("new data is on the page, and the banner or the 'looking for new data' line still shows")

    # A planned release's file is an hour old at most by design (27 minutes here): that isn't a run gone
    # missing, so no banner, and there's nothing for "Refresh now" to refresh.
    page.select_option("#release-select", "v1.2.0")
    expect_release(page, "v1.2.0")
    if banner.is_visible() or page.locator(".masthead a.refresh").count() != 0:
        raise AssertionError(
            f"the planned release: banner {banner.is_visible()}, "
            f"{page.locator('.masthead a.refresh').count()} 'Refresh now' links beside its age"
        )


def check_release_dashboard_budget_note(page: Page, site: str, offline: bool) -> None:
    """With the API budget gone the workflow publishes the last data again with the time the budget returns
    (#4726). The page says so until then, in place of the stale banner, and stops when new data arrives."""
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    generated = datetime.datetime.fromisoformat(index["generated_at"].replace("Z", "+00:00"))
    until = generated + datetime.timedelta(minutes=46)
    # What the data host serves: the fixtures, with these fields over index.json's own.
    served: dict[str, str] = {}

    def answer(route: Route) -> None:
        match = RELEASE_DATA.match(route.request.url)
        path = RELEASE_FIXTURES / match.group(1) if match else None
        if path is None or not path.is_file():
            route.fulfill(status=404, body="", headers={"Access-Control-Allow-Origin": "*"})
            return
        data = json.loads(path.read_text(encoding="utf-8"))
        if path.name == "index.json":
            data.update(served)
        elif "generated_at" in served and data.get("milestone", {}).get("title") == index["current"]:
            data["generated_at"] = served["generated_at"]
        route.fulfill(status=200, content_type="application/json", body=json.dumps(data), headers={"Access-Control-Allow-Origin": "*"})

    def stamp(moment: datetime.datetime) -> str:
        return moment.strftime("%Y-%m-%dT%H:%M:%SZ")

    page.route("https://raw.githubusercontent.com/**", answer)
    # Ten minutes after the data was read: nothing is stale, and nothing is said of the budget yet.
    page.clock.install(time=generated + datetime.timedelta(minutes=10))
    page.goto(site + "release/")
    expect_release(page, index["current"])
    note = page.locator(".banner.budget")
    stale = page.locator(".banner.stale")
    refresh = page.locator(".masthead a.refresh")
    watching = page.locator(".watching")
    if note.is_visible() or stale.is_visible():
        raise AssertionError("fresh data with the budget in hand shows a banner")
    # "Refresh now" is clicked, as someone would on a page that looks behind: the page looks every 30 seconds.
    page.evaluate(
        "() => document.addEventListener('click', event => {"
        " if (event.target.closest('a.refresh')) event.preventDefault(); }, true)"
    )
    refresh.click()
    watching.wait_for(state="visible", timeout=TIMEOUT_MS)

    # A time further ahead than a budget's hour is a mistake: the page doesn't take it, or it would hold the
    # warning about old data back until then. It goes on looking, as "Refresh now" asked.
    served["budget_until"] = stamp(generated + datetime.timedelta(hours=3))
    with page.expect_response(lambda response: response.url.endswith("/index.json")):
        page.clock.fast_forward(6 * 60 * 1000)
    page.wait_for_load_state("networkidle", timeout=TIMEOUT_MS)
    if note.is_visible() or not watching.is_visible():
        raise AssertionError("a budget note three hours ahead is shown, or stopped the page looking for new data")

    # The budget goes: the same data comes again with the time it returns. The page's next look shows it.
    served["budget_until"] = stamp(until)
    page.clock.fast_forward(6 * 60 * 1000)
    note.wait_for(state="visible", timeout=TIMEOUT_MS)
    text = note.text_content() or ""
    if "GitHub's API budget for this repository is used up until" not in text or "nothing can refresh these figures before then. The last run was at" not in text:
        raise AssertionError(f"the budget note reads {text!r}")
    told = page.evaluate("iso => new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' }).format(new Date(iso))", stamp(until))
    if note.locator("strong").text_content() != told:
        raise AssertionError(f"the budget note gives {note.locator('strong').text_content()!r}, not {told!r}")
    # The note says nothing can refresh the figures, so the page doesn't offer to, or go on looking for them.
    if refresh.is_visible() or watching.is_visible():
        raise AssertionError("beside the budget note the page still offers 'Refresh now' or says it is looking for new data")

    # Past 25 minutes the data is stale, but the reason is known: the note stands, and the stale banner, whose
    # "Refresh now" couldn't help, stays away.
    page.clock.fast_forward(9 * 60 * 1000)
    if not note.is_visible() or stale.is_visible():
        raise AssertionError("31 minutes on with the budget still gone, the page shows the stale banner or no note")

    # The time passes and no new data has come yet: the note goes, and the stale banner says what is left to say.
    page.clock.fast_forward(16 * 60 * 1000)
    stale.wait_for(state="visible", timeout=TIMEOUT_MS)
    if note.is_visible():
        raise AssertionError("the budget note is still shown after the time it gave")
    if not refresh.is_visible():
        raise AssertionError("the budget is back and 'Refresh now' isn't")

    # New data, without the note: nothing is shown.
    served.clear()
    served["generated_at"] = stamp(generated + datetime.timedelta(minutes=49))
    page.clock.fast_forward(6 * 60 * 1000)
    stale.wait_for(state="hidden", timeout=TIMEOUT_MS)
    if note.is_visible():
        raise AssertionError("new data came and the budget note is still shown")
    page.unroute("https://raw.githubusercontent.com/**")


# GitHub's public API, which a late dashboard asks what its refresh workflow is doing (#4734).
GITHUB_API = "https://api.github.com/**"


def serve_runs(page: Page, runs: list[dict] | None, calls: list[str] | None = None, reset: int | None = None,
               published: dict | None = None) -> None:
    """Answers the dashboard's questions to GitHub's API: the published index with @p published (the fixtures'
    own when None, so nothing newer is published), and the workflow's runs with @p runs. Refuses both (the
    hourly limit without a token) when @p runs is None, saying the limit resets at @p reset (seconds since
    1970) when that is given. Every check gets an empty run list by default, so none reaches the real API."""

    def answer(route: Route) -> None:
        url = route.request.url
        if calls is not None:
            calls.append(url)
        headers = {"Access-Control-Allow-Origin": "*"}
        if runs is None:
            if reset is not None:
                headers.update({"x-ratelimit-reset": str(reset), "Access-Control-Expose-Headers": "x-ratelimit-reset"})
            route.fulfill(status=403, content_type="application/json", body='{"message": "API rate limit exceeded"}', headers=headers)
        elif "/contents/index.json" in url:
            index = published if published is not None else json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
            route.fulfill(status=200, content_type="application/json", body=json.dumps(index), headers=headers)
        else:
            route.fulfill(status=200, content_type="application/json", body=json.dumps({"workflow_runs": runs}), headers=headers)

    page.unroute(GITHUB_API)
    page.route(GITHUB_API, answer)


class PageClock:
    """Moves a page's installed clock on by half minutes, as a page left open lives through them, and waits for
    what the page asked in each to be answered before the next. The page gives up on an answer after 20
    seconds of its own clock, so the clock mustn't run on ahead of the network. (`wait_for_load_state` is no
    help here: a page that has been idle once stays "networkidle".)"""

    def __init__(self, page: Page) -> None:
        self.page = page
        self.started = 0
        self.ended = 0
        page.on("request", self._on_start)
        page.on("requestfinished", self._on_end)
        page.on("requestfailed", self._on_end)

    def _on_start(self, request: Request) -> None:
        self.started += 1

    def _on_end(self, request: Request) -> None:
        self.ended += 1

    def settle(self) -> None:
        """Returns once nothing the page asked is unanswered, and nothing new was asked for a moment after."""
        quiet = 0
        for _ in range(int(TIMEOUT_MS / 40)):
            before = self.started
            self.page.wait_for_timeout(40)
            quiet = quiet + 1 if self.started == before and self.started == self.ended else 0
            if quiet >= 3:
                return
        raise AssertionError("the page's requests didn't settle")

    def minutes(self, since: datetime.datetime) -> float:
        """The page's clock, in minutes after @p since."""
        return (self.page.evaluate("() => Date.now()") / 1000 - since.timestamp()) / 60

    def forward(self, minutes: float) -> None:
        for _ in range(round(minutes * 2)):
            self.page.clock.run_for(30 * 1000)
            self.settle()

    def until(self, done: Callable[[], bool], what: str, limit: float = 8) -> None:
        """Half a minute at a time until @p done, for @p limit minutes at the most."""
        for _ in range(round(limit * 2)):
            if done():
                return
            self.page.clock.run_for(30 * 1000)
            self.settle()
        if not done():
            raise AssertionError(f"{limit:g} minutes on, {what}")


def check_release_dashboard_late_reason(page: Page, site: str, offline: bool) -> None:
    """Past ten minutes the page asks GitHub what is published and what the refresh is doing, and says it
    (#4734, #4741)."""
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    generated = datetime.datetime.fromisoformat(index["generated_at"].replace("Z", "+00:00"))
    serve_release_data(page)
    clock = PageClock(page)

    def at(minutes: float) -> str:
        return (generated + datetime.timedelta(minutes=minutes)).strftime("%Y-%m-%dT%H:%M:%SZ")

    def run(status: str, conclusion: str | None, created: float, updated: float, event: str = "schedule") -> dict:
        return {"status": status, "conclusion": conclusion, "created_at": at(created), "updated_at": at(updated), "event": event}

    def told(minutes: float, date: bool = False) -> str:
        parts = "{ month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }" if date else "{ hour: 'numeric', minute: '2-digit' }"
        return page.evaluate(f"iso => new Intl.DateTimeFormat(undefined, {parts}).format(new Date(iso))", at(minutes))

    calls: list[str] = []
    why = page.locator(".banner.late")
    stale = page.locator(".banner.stale")
    refresh = page.locator(".masthead a.refresh")

    def asks() -> int:
        return sum(1 for url in calls if "/contents/index.json" in url)

    def next_answer(words: str) -> None:
        """Goes on until the page has asked GitHub once more, and checks what it then says."""
        before = asks()
        clock.until(lambda: asks() > before, "the page hasn't asked GitHub again")
        clock.settle()
        shown = (why.text_content() or "") if why.is_visible() else ""
        if not shown.startswith(words):
            raise AssertionError(f"the page should say {words!r}; it says {shown or 'nothing'!r}")

    # The run that read the page's figures: created five and a half minutes before, done 18 seconds after.
    theirs = run("completed", "success", -5.5, 0.3)
    # #4741, as it happened on the live page: a run created before that read which only checked and stopped,
    # and ended nearly nine minutes after the read because it too waited for a runner. A success like any
    # other in this list, and it published nothing.
    stopped = run("completed", "success", -1.4, 8.8)
    # A pull request's run of the same workflow, from a fork's `main`: it never publishes, and isn't counted.
    proof = run("completed", "success", 8, 9, event="pull_request")

    # Nine minutes old: not late, and GitHub isn't asked.
    serve_runs(page, [run("queued", None, 2, 6), proof, stopped, theirs], calls)
    page.clock.install(time=generated + datetime.timedelta(minutes=9))
    page.goto(site + "release/")
    expect_release(page, index["current"])
    clock.settle()
    if calls or why.is_visible():
        raise AssertionError(f"figures nine minutes old had the page ask GitHub ({len(calls)} times) or show a reason")

    # Past ten minutes: nothing newer is published, and a run has been queued since minute 6. It is waiting for
    # a runner, and the page says so, with no "Refresh now": a manual run would cancel the one that is waiting.
    next_answer("A refresh has been waiting for a GitHub runner since")
    first_asked = clock.minutes(generated)
    if not 10 < first_asked < 11.5:
        raise AssertionError(f"the page first asked GitHub {first_asked:.1f} minutes after its figures were read, not just past ten")
    if "Starting another run won't help" not in (why.text_content() or ""):
        raise AssertionError(f"with a run queued, the page says {why.text_content()!r}")
    if why.locator("strong").text_content() != told(6):
        raise AssertionError(f"the wait is said to be since {why.locator('strong').text_content()!r}, not {told(6)!r}")
    if refresh.is_visible() or stale.is_visible():
        raise AssertionError("with a refresh waiting, the page still offers 'Refresh now' or shows the stale banner")
    # It asked two things, once each: what is published on the data's branch, then what the workflow's runs
    # on main are doing.
    if len(calls) != 2 or "/contents/index.json?ref=dashboard-data" not in calls[0] or "/actions/workflows/release-dashboard.yml/runs" not in calls[1] or "branch=main" not in calls[1]:
        raise AssertionError(f"the page asked GitHub {calls}")
    # A look at a shipped release and back, as someone comparing releases does. The answer is about what is
    # published, not about the release on screen: the note is there again at once, "Refresh now" is still
    # away, and GitHub wasn't asked a second time (the review of #4741: the note went blank for minutes).
    shipped = next(release["title"] for release in index["releases"] if release["stage"] == "released")
    page.locator("#release-select").select_option(shipped)
    expect_release(page, shipped)
    clock.settle()
    if why.is_visible():
        raise AssertionError("a shipped release, which never refreshes, shows the late note")
    page.locator("#release-select").select_option(index["current"])
    expect_release(page, index["current"])
    clock.settle()
    if not why.is_visible() or refresh.is_visible() or len(calls) != 2:
        raise AssertionError(f"back from a shipped release the note is gone, or 'Refresh now' is back, or GitHub was asked again ({len(calls)} requests)")
    # It asks again no sooner than five minutes after the last time, however often the page's clock ticks.
    clock.forward(4)
    if len(calls) != 2:
        raise AssertionError(f"four minutes later the page had asked GitHub {len(calls)} times")

    # The newest run failed: the page says that, and links the runs.
    serve_runs(page, [run("completed", "failure", 14, 15), theirs], calls)
    next_answer("The last refresh failed at")
    second_asked = clock.minutes(generated)
    if second_asked - first_asked < 4.9:
        raise AssertionError(f"the page asked GitHub again after {second_asked - first_asked:.1f} minutes, not five")
    link = why.locator("a")
    if why.locator("strong").text_content() != told(15) or link.count() != 1 or not (link.get_attribute("href") or "").endswith("/actions/workflows/release-dashboard.yml"):
        raise AssertionError(f"with the last refresh failed, the page says {why.text_content()!r}")
    if stale.is_visible() or not refresh.is_visible():
        raise AssertionError(f"at {second_asked:.0f} minutes with refreshes failing, the stale banner shows early or 'Refresh now' is gone")
    page.evaluate("() => { window.__lateLink = document.querySelector('.banner.late a'); window.__lateLink.focus(); }")
    # Past 25 minutes they are still failing. The stale banner comes as it always did, beside the reason, and
    # the note wasn't rebuilt for saying the same thing: its link is the same node, with the keyboard still on it.
    clock.until(lambda: clock.minutes(generated) > 26, "the page's clock hasn't passed 26 minutes", limit=12)
    if not stale.is_visible() or not why.is_visible():
        raise AssertionError("at 26 minutes with refreshes failing, the stale banner or the reason is missing")
    if not page.evaluate("() => window.__lateLink === document.querySelector('.banner.late a') && document.activeElement === window.__lateLink"):
        raise AssertionError("the note was rebuilt between ticks that said the same thing")

    # A run is under way: a refresh is running. The stale banner, which offers "Refresh now", goes while one is.
    serve_runs(page, [run("in_progress", None, 2, 26), run("completed", "cancelled", 1, 1), theirs], calls)
    next_answer("A refresh is running now")
    if refresh.is_visible() or stale.is_visible():
        raise AssertionError("with a refresh running, the page offers 'Refresh now' or shows the stale banner")

    # Nothing running and nothing newer published. The page doesn't say none has started (the run that only
    # checked did): it says nothing has been published since the last run, with that run's date and time. Now
    # "Refresh now" is what helps, in the note and beside the page's age.
    serve_runs(page, [run("completed", "cancelled", 16, 16), stopped, theirs], calls)
    next_answer("Nothing has been published since the last run at")
    if why.locator("strong").text_content() != told(0, date=True) or why.locator("a.refresh").count() != 1 or not refresh.is_visible():
        raise AssertionError(f"with nothing published, the page says {why.text_content()!r} and shows {why.locator('a.refresh').count()} links in it")
    # A planned release has no "Refresh now", in the note either, and the note doesn't call the last run's
    # time the time of its figures, which are older by design.
    planned = next((release["title"] for release in index["releases"] if release["stage"] == "planned"), None)
    if planned is None:
        raise AssertionError("the fixtures hold no planned release: tests/tools/test_release_dashboard.py")
    page.locator("#release-select").select_option(planned)
    expect_release(page, planned)
    clock.settle()
    if not why.is_visible() or why.locator("a.refresh").count() != 0 or "these figures" in (why.text_content() or ""):
        raise AssertionError(f"on a planned release the note reads {why.text_content()!r} with {why.locator('a.refresh').count()} 'Refresh now' links")
    page.locator("#release-select").select_option(index["current"])
    expect_release(page, index["current"])
    clock.settle()

    # GitHub refuses the question (the hourly limit without a token) and says when the limit resets: fourteen
    # minutes from now, well past the five the page would otherwise wait. The page says nothing of its own,
    # keeps the stale banner, and doesn't ask again before the reset: not even its second question, after the
    # first was refused.
    resets = clock.minutes(generated) + 14
    serve_runs(page, None, calls, reset=int((generated + datetime.timedelta(minutes=resets)).timestamp()))
    before = len(calls)
    clock.until(lambda: len(calls) > before, "the page hasn't asked GitHub again")
    clock.settle()
    if len(calls) != before + 1 or why.is_visible() or not stale.is_visible():
        raise AssertionError(f"refused, the page asked {len(calls) - before} questions, or shows a reason, or lost the stale banner")
    clock.until(lambda: clock.minutes(generated) > resets - 1, "the page's clock hasn't reached the reset", limit=15)
    if len(calls) != before + 1:
        raise AssertionError(f"before the limit reset the page asked GitHub {len(calls) - before - 1} more times")
    # After the reset it asks again.
    serve_runs(page, [run("in_progress", None, 40, 41), theirs], calls)
    next_answer("A refresh is running now")


def check_release_dashboard_late_first_look(page: Page, site: str, offline: bool) -> None:
    """Three things a late page gets right on its first look, where the moment it asks GitHub is known and its
    own next look at the data is five minutes off (#4741): a run queued for seconds isn't "waiting for a
    runner"; and when what is published is newer than the page's copy, or carries a budget note the copy
    lacks, only the copy is old, so the page asks the data host again and says nothing of runs."""
    index = json.loads((RELEASE_FIXTURES / "index.json").read_text(encoding="utf-8"))
    generated = datetime.datetime.fromisoformat(index["generated_at"].replace("Z", "+00:00"))

    def at(minutes: float) -> str:
        return (generated + datetime.timedelta(minutes=minutes)).strftime("%Y-%m-%dT%H:%M:%SZ")

    calls: list[str] = []
    looks: list[str] = []  # each time the page asks the data host for index.json
    page.on("request", lambda request: looks.append(request.url) if request.url.startswith("https://raw.githubusercontent.com/") and request.url.endswith("/index.json") else None)
    why = page.locator(".banner.late")
    serve_release_data(page)
    clock = PageClock(page)
    waiting = {"status": "queued", "conclusion": None, "event": "schedule", "created_at": at(2), "updated_at": at(6)}

    def look() -> None:
        calls.clear()
        looks.clear()
        page.goto(site + "release/")
        expect_release(page, index["current"])
        clock.settle()

    # Twelve minutes old at the first look, so the page asks GitHub at once. The one run was queued 18 seconds
    # before: every run is, for a few seconds, on its way to a runner. A refresh is running.
    serve_runs(page, [{**waiting, "created_at": at(11.6), "updated_at": at(11.7)}], calls)
    page.clock.install(time=generated + datetime.timedelta(minutes=12))
    look()
    if not (why.text_content() or "").startswith("A refresh is running now") or not why.is_visible():
        raise AssertionError(f"with a run queued 18 seconds ago, the page says {why.text_content() if why.is_visible() else 'nothing'!r}")

    published = {
        "newer figures": {**index, "generated_at": at(11)},
        "a budget note the page's copy lacks": {**index, "budget_until": at(50)},
    }
    for name, on_github in published.items():
        # A run has been queued for minutes, and it isn't the reason: one question to GitHub, none about runs,
        # no note, and the data host asked a second time (the first is the page loading).
        serve_runs(page, [waiting], calls, published=on_github)
        look()
        if len(calls) != 1 or "/contents/index.json" not in calls[0]:
            raise AssertionError(f"with {name} published, the page asked GitHub {calls}")
        if why.is_visible() or len(looks) != 2:
            raise AssertionError(f"with {name} published, the page says {why.text_content() if why.is_visible() else 'nothing'!r} "
                                 f"and asked the data host {len(looks)} times, not twice")


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
    ("the release dashboard says when the API budget is used up, and until when", check_release_dashboard_budget_note),
    ("a late release dashboard says why: a refresh waiting for a runner, running, failed, or none published", check_release_dashboard_late_reason),
    ("a late release dashboard's first look: a run just queued is running, a published budget note is the reason", check_release_dashboard_late_first_look),
    ("the release dashboard flags old data and looks for new data after 'Refresh now'", check_release_dashboard_refresh),
    ("a lane's list on the release dashboard ends with what waits on the maintainer", check_release_dashboard_lane_all_waiting),
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
                serve_runs(page, [])  # no check reaches GitHub's real API (#4734)
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
