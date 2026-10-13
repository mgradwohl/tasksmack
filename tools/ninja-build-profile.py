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

import sys
from pathlib import Path


def kind(output: str) -> str:
    lower = output.lower()
    if lower.endswith(".ddi"):
        return "module scan"
    if lower.endswith((".obj", ".o", ".res")):
        return "compile"
    if lower.endswith((".exe", ".dll", ".so")) or "/bin/" in lower.replace("\\", "/"):
        return "link"
    if lower.endswith((".lib", ".a")):
        return "static library"
    return "other"


def main() -> int:
    args = sys.argv[1:]
    top = 8
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i : i + 2]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2

    log = Path(args[0]) / ".ninja_log"
    if not log.is_file():
        return 0

    # One line per output; an edge with several outputs (an exe and its .pdb, an object and its
    # .ddi) repeats its start/end times and command hash, so key edges on those.
    edges: dict[tuple[str, str, str], list[str]] = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        start, end, _mtime, output, cmd_hash = parts[:5]
        edges.setdefault((start, end, cmd_hash), []).append(output)
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
        print(f"Slowest links (top {top}):")
        print()
        print("| Link | Time (s) |")
        print("|---|---:|")
        for seconds, name in sorted(links, reverse=True)[:top]:
            print(f"| `{name}` | {seconds:.1f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
