#!/usr/bin/env python3
"""
tools/docs-sri.py -- Subresource Integrity for the docs site's jsDelivr pins (#3167).

Usage:
    python -I tools/docs-sri.py --check   [--root DIR]
    python -I tools/docs-sri.py --update  [--root DIR]

docs/index.html loads docsify, its plugins, mermaid and prismjs from cdn.jsdelivr.net, each pinned to a
version in its URL. Every one of those <script> and <link> tags carries integrity="sha384-..." and
crossorigin="anonymous", so a tampered CDN response is refused by the browser instead of running on the
published site.

docs/cdn-integrity.json is the lockfile: each pinned URL and its hash.

  --check   (offline; the pre-commit hook) fails when a jsDelivr tag has no integrity or crossorigin
            attribute, when its integrity differs from the lockfile's, or when the lockfile and the page
            list different URLs. A version bump (Renovate's, or by hand) changes the URL, so it fails
            here until the hashes are regenerated: a stale hash can't reach the site.
  --update  (network) downloads every pinned URL from jsDelivr, writes its sha384 to the lockfile, and
            rewrites the tags' integrity and crossorigin attributes. Run it after changing a version.

Exits 1 on any failure, 0 otherwise. `--root` points at another tree, for testing.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
import urllib.request
from collections.abc import Callable
from pathlib import Path

PAGE = Path("docs/index.html")
LOCK = Path("docs/cdn-integrity.json")
CDN_PREFIX = "https://cdn.jsdelivr.net/npm/"

# A <script ...> or <link ...> start tag, possibly over several lines.
TAG = re.compile(r"<(?P<name>script|link)\b(?P<attrs>[^>]*?)(?P<end>\s*/?>)", re.IGNORECASE | re.DOTALL)
URL_ATTR = re.compile(r"""\b(?:src|href)\s*=\s*["'](?P<url>[^"']+)["']""", re.IGNORECASE)
INTEGRITY_ATTR = re.compile(r"""\s+integrity\s*=\s*["'](?P<value>[^"']*)["']""", re.IGNORECASE)
CROSSORIGIN_ATTR = re.compile(r"""\s+crossorigin\s*=\s*["'](?P<value>[^"']*)["']""", re.IGNORECASE)


def cdn_tags(html: str) -> list[re.Match[str]]:
    """The <script>/<link> tags that load from jsDelivr, in page order."""
    tags = []
    for tag in TAG.finditer(html):
        url = URL_ATTR.search(tag.group("attrs"))
        if url and url.group("url").startswith(CDN_PREFIX):
            tags.append(tag)
    return tags


def tag_url(tag: re.Match[str]) -> str:
    match = URL_ATTR.search(tag.group("attrs"))
    assert match is not None
    return match.group("url")


def sri(content: bytes) -> str:
    """The integrity value for @p content: "sha384-" and its base64 digest."""
    return "sha384-" + base64.b64encode(hashlib.sha384(content).digest()).decode("ascii")


def fetch(url: str) -> bytes:
    """@p url's bytes from the network."""
    with urllib.request.urlopen(url, timeout=30) as response:  # noqa: S310 - fixed https CDN URLs
        return response.read()


def check(root: Path) -> list[str]:
    """Every problem with the page's integrity attributes against the lockfile; empty when none."""
    html = (root / PAGE).read_text(encoding="utf-8")
    try:
        lock: dict[str, str] = json.loads((root / LOCK).read_text(encoding="utf-8"))
    except FileNotFoundError:
        lock = {}
    problems = []
    seen = set()
    for tag in cdn_tags(html):
        url = tag_url(tag)
        seen.add(url)
        integrity = INTEGRITY_ATTR.search(tag.group("attrs"))
        crossorigin = CROSSORIGIN_ATTR.search(tag.group("attrs"))
        if url not in lock:
            problems.append(f"{url}: not in {LOCK} (a new or changed version?)")
        if integrity is None:
            problems.append(f"{url}: no integrity attribute")
        elif url in lock and integrity.group("value") != lock[url]:
            problems.append(f"{url}: integrity differs from {LOCK}")
        if crossorigin is None or crossorigin.group("value") != "anonymous":
            problems.append(f'{url}: needs crossorigin="anonymous" (a CDN response is cross-origin)')
    for url in sorted(set(lock) - seen):
        problems.append(f"{url}: in {LOCK} but no longer on the page")
    return problems


def update(root: Path, download: Callable[[str], bytes] = fetch) -> None:
    """Hashes every pinned URL into the lockfile and rewrites the page's integrity attributes."""
    page = root / PAGE
    html = page.read_text(encoding="utf-8")
    hashes = {url: sri(download(url)) for url in dict.fromkeys(tag_url(tag) for tag in cdn_tags(html))}

    def rewrite(tag: re.Match[str]) -> str:
        url_match = URL_ATTR.search(tag.group("attrs"))
        if not url_match or not url_match.group("url").startswith(CDN_PREFIX):
            return tag.group(0)
        attrs = CROSSORIGIN_ATTR.sub("", INTEGRITY_ATTR.sub("", tag.group("attrs")))
        added = f'integrity="{hashes[url_match.group("url")]}" crossorigin="anonymous"'
        if "\n" in tag.group(0):
            # A tag laid out one attribute per line keeps that layout.
            indent = re.search(r"\n([ \t]*)\S", tag.group(0))
            pad = indent.group(1) if indent else "    "
            attrs = attrs.rstrip() + "\n" + pad + added.replace(" crossorigin", "\n" + pad + "crossorigin")
        else:
            attrs = attrs.rstrip() + " " + added
        return f"<{tag.group('name')}{attrs}{tag.group('end')}"

    page.write_text(TAG.sub(rewrite, html), encoding="utf-8")
    (root / LOCK).write_text(json.dumps(dict(sorted(hashes.items())), indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="verify the page against the lockfile (offline)")
    mode.add_argument("--update", action="store_true", help="download, hash, and rewrite (network)")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    if args.update:
        update(args.root)
        print(f"Updated {LOCK} and the integrity attributes in {PAGE}.")
        return 0
    problems = check(args.root)
    for problem in problems:
        print(f"docs-sri: {problem}", file=sys.stderr)
    if problems:
        print("Run `python -I tools/docs-sri.py --update` after changing a jsDelivr version (#3167).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
