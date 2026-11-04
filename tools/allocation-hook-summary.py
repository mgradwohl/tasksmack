#!/usr/bin/env python3
"""
tools/allocation-hook-summary.py -- Which tests ran with the allocation-failure hook armed (#3342).

Usage:
    allocation-hook-summary.py [--require] <hook-log> <cases.xml>

tests/Domain/AllocationFailureHook.cpp appends "Suite.Case" to <hook-log> (the file
TASKSMACK_ALLOCATION_HOOK_LOG names) the first time each test arms the hook. <cases.xml> is the
per-case JUnit report tools/merge-gtest-results.py writes. This prints a Markdown summary for the
Job Summary: how many cases armed the hook, and how many skipped because the hook is compiled out
(their skip message names operator new, as TASKSMACK_NO_ALLOCATOR_HOOK's reasons do).

--require is for the builds where the hook is compiled in (Linux debug and release): it exits 1 when
no case armed it or any case skipped for want of it, so a hook that silently stopped running fails
the job instead of passing with its tests skipped. Without --require the summary is informational.

When <cases.xml> doesn't exist (the build failed before the tests ran), nothing is printed and the
exit code is 0, so the CI step never hides the build's own failure.
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Every TASKSMACK_NO_ALLOCATOR_HOOK reason names it ("the sanitizer runtime owns operator new", "...
# failing a replaced operator new on demand"), and a hook test skips with that reason as its message.
HOOK_SKIP_MARKER = "operator new"


def armed_cases(log: Path) -> list[str]:
    """The distinct cases the log names, sorted; none when the log doesn't exist (no case armed it)."""
    if not log.is_file():
        return []
    return sorted({line.strip() for line in log.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()})


def hook_skipped_cases(cases_xml: Path) -> list[str]:
    """The cases that skipped because the hook is compiled out, as "Suite.Case", sorted."""
    skipped = set()
    for case in ET.parse(cases_xml).getroot().iter("testcase"):
        for skip in case.iter("skipped"):
            text = (skip.get("message") or "") + (skip.text or "")
            if HOOK_SKIP_MARKER in text:
                skipped.add(f"{case.get('classname')}.{case.get('name')}")
    return sorted(skipped)


def summary(armed: list[str], skipped: list[str]) -> str:
    suites = {name.split(".", 1)[0] for name in armed}
    lines = [
        f"**Allocation-failure hook (#3342):** armed in {len(armed)} test case(s) across {len(suites)} suite(s); "
        f"{len(skipped)} case(s) skipped because the hook is compiled out.",
    ]
    for title, names in (("Cases that armed the hook", armed), ("Cases skipped without the hook", skipped)):
        if names:
            lines += ["", f"<details><summary>{title}</summary>", ""]
            lines += [f"- `{name}`" for name in names]
            lines += ["", "</details>"]
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    require = "--require" in argv
    paths = [arg for arg in argv if arg != "--require"]
    if len(paths) != 2:
        print(__doc__.strip().splitlines()[3].strip(), file=sys.stderr)
        return 2
    log, cases_xml = Path(paths[0]), Path(paths[1])
    if not cases_xml.is_file():
        return 0
    armed = armed_cases(log)
    skipped = hook_skipped_cases(cases_xml)
    print(summary(armed, skipped), end="")
    if require and not armed:
        print("::error title=allocation-failure hook::No test armed the allocation-failure hook, which this build compiles in.")
        return 1
    if require and skipped:
        print(f"::error title=allocation-failure hook::{len(skipped)} test case(s) skipped for want of the allocation-failure hook, which this build compiles in.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
