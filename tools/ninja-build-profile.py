#!/usr/bin/env python3
"""
tools/ninja-build-profile.py -- Where a Ninja build spent its time, as a Markdown summary (#1875).

Usage:
    ninja-build-profile.py <build-dir> [--top N]

Reads <build-dir>/.ninja_log and prints, per kind of build edge (C++ module dependency scans,
compiles, static libraries, executable/DLL links, everything else), the edge count, the summed
edge time and the longest edge, then the N slowest links. CI appends it to the job summary after a
build, so a change to compile or link flags can be measured from one run's summary instead of
from step durations alone.

Summed times are CPU-ish (edges run in parallel), so compare them between runs, not with the
step's wall time. Links mostly run alone at the end of the build, so their times are close to the
wall time they cost. A missing or empty log prints nothing and exits 0: this is a report, never a
reason to fail a build.
"""

import argparse
import posixpath
import sys
from pathlib import Path


def kind(output: str) -> str:
    # Ninja logs outputs relative to the build directory ("bin/TaskSmack", "tests/TaskSmackTests"),
    # so anchor the path before looking for a bin/ component.
    lower = "/" + output.lower().replace("\\", "/")
    name = posixpath.basename(lower)
    if lower.endswith(".ddi"):
        return "module scan"
    if lower.endswith((".obj", ".o", ".res")):
        return "compile"
    if lower.endswith((".exe", ".dll", ".so")) or ".so." in name or "/bin/" in lower:
        return "link"
    if lower.endswith((".lib", ".a")):
        return "static library"
    # Linux executables have no suffix (tests/TaskSmackTests); CMake's own extensionless outputs
    # live under CMakeFiles/.
    if "." not in name and "/cmakefiles/" not in lower:
        return "link"
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", help="the Ninja build directory holding .ninja_log")
    parser.add_argument("--top", type=int, default=8, help="how many of the slowest links to list (default: 8)")
    args = parser.parse_args()

    log = Path(args.build_dir) / ".ninja_log"
    if not log.is_file():
        return 0

    # .ninja_log is append-only, so a reused build directory lists an output once per build that
    # rebuilt it; keep each output's latest line.
    latest: dict[str, tuple[str, str, str]] = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        start, end, _mtime, output, cmd_hash = parts[:5]
        latest.pop(output, None)  # re-insert, so dict order follows the log
        latest[output] = (start, end, cmd_hash)

    # An edge with several outputs (an exe and its .pdb, an object and its .ddi) repeats its
    # start/end times and command hash, so key edges on those.
    edges: dict[tuple[str, str, str], list[str]] = {}
    for output, key in latest.items():
        edges.setdefault(key, []).append(output)
    if not edges:
        return 0

    rows: dict[str, list[float]] = {}
    links: list[tuple[float, str]] = []
    for (start, end, _), outputs in edges.items():
        seconds = (int(end) - int(start)) / 1000.0
        # The edge's kind is that of its most specific output: a link edge also lists its .pdb or
        # import .lib, a compile edge with a scan lists its .obj.
        kinds = {kind(o) for o in outputs}
        for k in ("link", "compile", "module scan", "static library", "other"):
            if k in kinds:
                break
        rows.setdefault(k, []).append(seconds)
        if k == "link":
            name = next(o for o in outputs if kind(o) == "link")
            links.append((seconds, name))

    print("### Build profile (.ninja_log)")
    print()
    print("| Edge kind | Edges | Summed time (s) | Longest (s) |")
    print("|---|---:|---:|---:|")
    for k in ("module scan", "compile", "static library", "link", "other"):
        if k in rows:
            times = rows[k]
            print(f"| {k} | {len(times)} | {sum(times):.1f} | {max(times):.1f} |")
    if links:
        print()
        print(f"Slowest links (top {args.top}):")
        print()
        print("| Link | Time (s) |")
        print("|---|---:|")
        for seconds, name in sorted(links, reverse=True)[:args.top]:
            print(f"| `{name}` | {seconds:.1f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
