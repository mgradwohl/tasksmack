#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:-"$(pwd)"}

python3 - "$repo_root" <<'PY'
import sys
import re
import os
import pathlib
import urllib.parse
import collections

repo_root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.getcwd()).resolve()
md_files = list(repo_root.glob("*.md"))
# Include .github markdown files
if (repo_root / ".github").exists():
    md_files += list((repo_root / ".github").rglob("*.md"))

external_prefixes = ("http://", "https://", "mailto:", "data:")
anchor_cache: dict[str, set[str]] = {}


def slug(text: str) -> str:
    t = text.strip().lower().replace("`", "")
    t = re.sub(r"[^a-z0-9\s-]", "", t)
    t = re.sub(r"[\s-]+", "-", t).strip("-")
    return t


def anchors_for(path: pathlib.Path) -> set[str]:
    anchors: list[str] = []
    counts: collections.Counter[str] = collections.Counter()
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(#{1,6})\s+(.*?)(\s+#*\s*)?$", line.rstrip())
        if not m:
            continue
        title = m.group(2).strip()
        if not title:
            continue
        base = slug(title)
        if not base:
            continue
        n = counts[base]
        anchors.append(base if n == 0 else f"{base}-{n}")
        counts[base] += 1
    return set(anchors)


def get_anchors(path: pathlib.Path) -> set[str]:
    key = str(path.resolve())
    if key not in anchor_cache:
        anchor_cache[key] = anchors_for(path)
    return anchor_cache[key]


def add(kind: str, file: str, line: int, raw: str, detail: str):
    broken.append((kind, file, line, raw, detail))


def resolve_target(md_path: pathlib.Path, target: str) -> tuple[pathlib.Path, str]:
    path_part, anchor = target, ""
    if "#" in target:
        path_part, anchor = target.split("#", 1)
    path_part = urllib.parse.unquote(path_part)
    anchor = urllib.parse.unquote(anchor)
    if path_part.startswith("/"):
        target_path = repo_root / path_part.lstrip("/")
    else:
        target_path = (md_path.parent / path_part)
    return target_path, anchor


broken: list[tuple[str, str, int, str, str]] = []

for md in md_files:
    lines = md.read_text(encoding="utf-8").splitlines()

    # Reference definitions: [ref]: target
    ref_defs: dict[str, str] = {}
    for line in lines:
        m = re.match(r"^\s*\[([^\]]+)\]:\s*(\S+)(?:\s+\"[^\"]*\")?\s*$", line)
        if m:
            ref_defs[m.group(1).strip().lower()] = m.group(2).strip()

    for idx, line in enumerate(lines, start=1):
        # Inline links/images: [text](target)
        for m in re.finditer(r"!?\[[^\]]*\]\(([^)]+)\)", line):
            target = m.group(1).strip()
            if target.startswith("<") and target.endswith(">"):
                target = target[1:-1].strip()
            if " " in target and not target.startswith("#"):
                target = target.split(None, 1)[0]
            if not target:
                continue
            if any(target.startswith(p) for p in external_prefixes):
                continue
            # Anchor-only link
            if target.startswith("#") and len(target) > 1:
                anchor = urllib.parse.unquote(target[1:])
                if anchor and anchor not in get_anchors(md):
                    add("missing-anchor", str(md), idx, target, f"Missing anchor '#{anchor}' in {md}")
                continue
            if target.startswith("#"):
                continue
            target_path, anchor = resolve_target(md, target)
            # Drop query string
            if "?" in target_path.name:
                target_path = target_path.with_name(target_path.name.split("?", 1)[0])
            if not target_path.exists():
                add("missing-file", str(md), idx, target, f"Missing file '{target_path}'")
                continue
            full_target = target_path.resolve()
            if not str(full_target).startswith(str(repo_root)):
                continue
            if anchor and anchor not in get_anchors(full_target):
                add("missing-anchor", str(md), idx, target, f"Missing anchor '#{anchor}' in {full_target}")

        # Reference-style: [text][ref]
        for m in re.finditer(r"!?\[([^\]]+)\]\[([^\]]*)\]", line):
            text_label, ref = m.group(1), m.group(2)
            ref_key = (ref or text_label).strip().lower()
            if ref_key not in ref_defs:
                add("missing-refdef", str(md), idx, f"ref:{ref_key}", f"Missing reference definition for [{ref_key}]")
                continue
            target = ref_defs[ref_key]
            if any(target.startswith(p) for p in external_prefixes):
                continue
            target_path, anchor = resolve_target(md, target)
            if not target_path.exists():
                add("missing-file", str(md), idx, target, f"Missing file '{target_path}' (ref [{ref_key}])")
                continue
            full_target = target_path.resolve()
            if anchor and anchor not in get_anchors(full_target):
                add("missing-anchor", str(md), idx, target, f"Missing anchor '#{anchor}' in {full_target} (ref [{ref_key}])")

print(f"Scanned {len(md_files)} markdown files (root + .github).")
print(f"Broken internal link findings: {len(broken)}")

if broken:
    broken.sort()
    kinds = sorted({k for k, *_ in broken})
    for kind in kinds:
        print(f"\n== {kind} ==")
        for b in sorted([x for x in broken if x[0] == kind], key=lambda t: (t[1], t[2]))[:200]:
            file_path = pathlib.Path(b[1])
            try:
                rel = file_path.resolve().relative_to(repo_root)
                rel_str = str(rel).replace('\\', '/')
            except Exception:
                rel_str = str(file_path)
            print(f"{rel_str}:{b[2]}: {b[3]} -> {b[4]}")
    sys.exit(1)

sys.exit(0)
PY
