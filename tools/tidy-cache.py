#!/usr/bin/env python3
"""
tools/tidy-cache.py -- Skip clang-tidy on translation units that already passed with identical inputs.

Usage:
    tools/tidy-cache.py select --cache-dir DIR --compdb FILE --clang-tidy EXE --repo-root DIR
                               [--salt TEXT]... [--salt-file PATH]... [--extra-arg ARG]...
                               [--prune] [--files-from PATH] [FILE...]

Prints one line per translation unit that still has to be analyzed, "<key><TAB><file>", and a
summary on stderr. The key is "-" when the file couldn't be keyed (it is analyzed, never recorded).
tools/clang-tidy.sh and tools/clang-tidy.ps1 call this when given --cache-dir / -CacheDir (#1785),
and after a clean run of a file (exit code 0, no "warning:" or "error:" in its output) write the
empty-ish marker file <cache-dir>/<key>. Only clean results are cached; a file with findings is
analyzed again every time, so its findings are always printed.

A key is a SHA-256 over everything that can change what clang-tidy reports for that file:
  - the clang-tidy build (`clang-tidy --version`), the runner image (ImageOS/ImageVersion, which
    decides the system and SDK headers), this script, and the caller's --salt-file/--salt values
    (the .clang-tidy config, the calling script, its header-filter regexes and check overrides);
  - the file's compilation-database entry (compiler, flags, defines, include paths) plus the
    --extra-arg values clang-tidy appends;
  - the path and full content of every file the translation unit includes, directly or not, as
    clang-scan-deps reports them for that exact command: project headers, generated headers,
    FetchContent dependencies, the standard library, the Windows SDK. Content, not preprocessed
    output, so a comment (a NOLINT) counts too;
  - every file in the repository (tracked or untracked, not ignored) whose name matches the name
    of one of those includes, so a new header that could shadow an existing one changes the key.
What it can't see is an include that was *not* found (a false `__has_include`): a new system
header changes the runner image version, and src/ only uses `__has_include` on system headers.

Any failure (no clang-scan-deps, a scan error, an unreadable file) only makes files unkeyed, which
means they are analyzed: a cache problem can cost time, never skip a check. --prune deletes markers
whose key no current file has (pass it on a whole-tree run, so the saved cache stays small).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FORMAT_VERSION = "tidy-cache v1"
KEY_RE = re.compile(r"^[0-9a-f]{64}$")


def norm(path: str, directory: str = "") -> str:
    return os.path.normcase(os.path.normpath(os.path.join(directory, path)))


def warn(message: str) -> None:
    print(f"tidy-cache: {message}", file=sys.stderr)


def find_scan_deps(clang_tidy: str) -> str | None:
    """clang-scan-deps from the same LLVM install as clang-tidy (same headers, same version)."""
    exe = ".exe" if os.name == "nt" else ""
    candidates = []
    for tidy in (clang_tidy, os.path.realpath(clang_tidy)):
        folder = os.path.dirname(tidy)
        suffix = Path(tidy).name.removesuffix(exe).removeprefix("clang-tidy")  # "-23" for clang-tidy-23
        candidates += [os.path.join(folder, f"clang-scan-deps{suffix}{exe}"), os.path.join(folder, f"clang-scan-deps{exe}")]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return shutil.which("clang-scan-deps")


def entry_command(entry: dict, extra_args: list[str]) -> dict:
    """The entry as clang-tidy sees it: its own command plus the appended --extra-arg values."""
    entry = dict(entry)
    if "arguments" in entry:
        entry["arguments"] = [*entry["arguments"], *extra_args]
    else:
        entry["command"] = " ".join([entry["command"], *(f'"{a}"' if " " in a else a for a in extra_args)])
    return entry


def scan_dependencies(scan_deps: str, entries: list[dict]) -> dict[str, list[str]]:
    """Normalized source path -> every file its translation unit includes (from clang-scan-deps)."""
    with tempfile.TemporaryDirectory(prefix="tidy-cache-") as temp:
        database = Path(temp) / "compile_commands.json"
        database.write_text(json.dumps(entries), encoding="utf-8")
        result = subprocess.run(
            [scan_deps, f"-compilation-database={database}", "-format=experimental-full",
             "-resource-dir-recipe=invoke-compiler", "-j", str(os.cpu_count() or 4)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
        )
    if result.returncode != 0:
        # Files that failed to scan are missing from the output and so stay unkeyed (analyzed).
        warn(f"clang-scan-deps exited with {result.returncode}: {result.stderr.strip()[:2000]}")
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        warn("clang-scan-deps output is not JSON; nothing is cached this run")
        return {}
    deps: dict[str, list[str]] = {}
    for unit in report.get("translation-units", []):
        for command in unit.get("commands", []):
            source = command.get("input-file")
            if source:
                deps.setdefault(norm(source), []).extend(command.get("file-deps", []))
    return deps


def repo_files_by_name(repo_root: str) -> dict[str, list[str]] | None:
    try:
        listing = subprocess.run(
            ["git", "-C", repo_root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            capture_output=True, check=True,
        ).stdout.decode("utf-8", errors="replace")
    except (OSError, subprocess.CalledProcessError) as error:
        warn(f"git ls-files failed ({error}); nothing is cached this run")
        return None
    by_name: dict[str, list[str]] = {}
    for path in filter(None, listing.split("\0")):
        by_name.setdefault(os.path.normcase(os.path.basename(path)), []).append(path)
    return by_name


class Hasher:
    def __init__(self) -> None:
        self.content: dict[str, str | None] = {}

    def file(self, path: str) -> str | None:
        if path not in self.content:
            try:
                self.content[path] = hashlib.sha256(Path(path).read_bytes()).hexdigest()
            except OSError:
                self.content[path] = None
        return self.content[path]


def salt(args: argparse.Namespace) -> str:
    digest = hashlib.sha256()

    def add(label: str, data: bytes) -> None:
        digest.update(f"{label}\0{len(data)}\0".encode())
        digest.update(data)

    add("format", FORMAT_VERSION.encode())
    add("self", Path(__file__).read_bytes())
    version = subprocess.run([args.clang_tidy, "--version"], capture_output=True, check=True).stdout
    add("clang-tidy", version)
    for name in ("ImageOS", "ImageVersion"):
        add(name, os.environ.get(name, "").encode())
    for path in args.salt_file:
        add("salt-file", Path(path).name.encode() + b"\0" + Path(path).read_bytes())
    for text in args.salt:
        add("salt", text.encode())
    for arg in args.extra_arg:
        add("extra-arg", arg.encode())
    return digest.hexdigest()


def select(args: argparse.Namespace) -> int:
    files = list(args.files)
    if args.files_from:
        files += [line.strip() for line in Path(args.files_from).read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    keys: dict[str, str] = {}  # file as given -> key

    try:
        base = salt(args)
        scan_deps = find_scan_deps(args.clang_tidy)
        if not scan_deps:
            raise RuntimeError("clang-scan-deps not found next to clang-tidy or on PATH")
        by_name = repo_files_by_name(args.repo_root)
        if by_name is None:
            raise RuntimeError("cannot list repository files")
        entries: dict[str, dict] = {}
        for entry in json.loads(Path(args.compdb).read_text(encoding="utf-8-sig")):
            entries.setdefault(norm(entry["file"], entry.get("directory", "")), entry)
        wanted = {norm(f): f for f in files}
        selected = [entry_command(entries[n], args.extra_arg) for n in wanted if n in entries]
        deps = scan_dependencies(scan_deps, selected)
        hasher = Hasher()
        for normalized, given in wanted.items():
            if normalized not in entries or normalized not in deps:
                continue
            digest = hashlib.sha256(base.encode())
            digest.update(json.dumps(entry_command(entries[normalized], args.extra_arg), sort_keys=True).encode())
            unique = sorted({norm(d) for d in deps[normalized]})
            ok = True
            names: set[str] = set()
            for dep in unique:
                content = hasher.file(dep)
                if content is None:
                    ok = False
                    break
                digest.update(f"\0dep\0{dep}\0{content}".encode())
                names.add(os.path.normcase(os.path.basename(dep)))
            if not ok:
                continue
            for name in sorted(names):
                for path in sorted(by_name.get(name, [])):
                    digest.update(f"\0name\0{path}".encode())
            keys[given] = digest.hexdigest()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        warn(f"{error}; every file is analyzed")
        keys = {}

    cache = Path(args.cache_dir)
    cache.mkdir(parents=True, exist_ok=True)
    if args.prune and keys:
        live = set(keys.values())
        for marker in cache.iterdir():
            if KEY_RE.match(marker.name) and marker.name not in live:
                marker.unlink(missing_ok=True)

    hits = 0
    for given in files:
        key = keys.get(given)
        if key and (cache / key).is_file():
            hits += 1
            continue
        print(f"{key or '-'}\t{given}")
    unkeyed = sum(1 for f in files if f not in keys)
    print(
        f"clang-tidy cache: {hits} of {len(files)} translation units unchanged since a clean run "
        f"(skipped), {len(files) - hits} to analyze ({unkeyed} not cacheable)",
        file=sys.stderr,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sel = sub.add_parser("select", help="print the files that still need clang-tidy, with their keys")
    sel.add_argument("--cache-dir", required=True)
    sel.add_argument("--compdb", required=True, help="the compile_commands.json clang-tidy uses")
    sel.add_argument("--clang-tidy", required=True)
    sel.add_argument("--repo-root", required=True)
    sel.add_argument("--salt", action="append", default=[])
    sel.add_argument("--salt-file", action="append", default=[])
    sel.add_argument("--extra-arg", action="append", default=[])
    sel.add_argument("--prune", action="store_true", help="delete markers no current file has")
    sel.add_argument("--files-from", help="read more files from this file, one per line")
    sel.add_argument("files", nargs="*")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")  # the callers split on LF
    return select(args)


if __name__ == "__main__":
    sys.exit(main())
