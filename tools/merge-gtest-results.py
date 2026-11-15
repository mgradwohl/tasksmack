#!/usr/bin/env python3
"""
tools/merge-gtest-results.py -- One JUnit report with a testcase per Google Test case (#1871).

Usage:
    merge-gtest-results.py [--later <gtest.xml>]... <ctest-junit.xml>... <gtest-results-dir> <output.xml>

ctest runs one entry per gtest suite (cmake/GTestSuites.cmake), so its --output-junit report has a
testcase per suite. Each entry also writes gtest's own XML report, one testcase per case, to
<gtest-results-dir>/<entry name as a C identifier>.xml. This merges the two: every gtest case from
the reports of the entries ctest ran, plus the ctest entries that have no gtest report (the script
tests) or did not pass (a crash or timeout leaves no report, and a failing entry's output names
the cases it ran). Shards of one suite are merged back into one testsuite.

--later names a gtest XML report from a step that ran after ctest and ran cases ctest skipped (the
real-system and root steps, which only run where CI asks, #3840). A later result for a case replaces
that case's skip, so it counts once, by its real result; a case ctest ran is never replaced, and a
later skip never replaces a result. A later case no earlier report has is added. A later report that
doesn't exist (the step didn't run) is skipped.

Several ctest reports (Windows ARM64 runs the script tests in a second ctest run, #2832) merge as
one. A ctest report that doesn't exist is skipped; when none does (the build failed before the tests
ran) nothing is written and the exit code is 0, so the CI step that runs this never hides the
build's own failure.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def c_identifier(name: str) -> str:
    """CMake's string(MAKE_C_IDENTIFIER): the file name cmake/GTestSuites.cmake gives a report."""
    ident = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return "_" + ident if ident[:1].isdigit() else ident


def passed(entry: ET.Element) -> bool:
    return entry.get("status") == "run" and entry.find("failure") is None and entry.find("error") is None


def is_skipped(case: ET.Element) -> bool:
    return case.find("skipped") is not None or case.get("status") in ("notrun", "disabled")


def apply_later(suites: dict[str, ET.Element], later: Path) -> None:
    """Fold a later gtest report in: its result replaces a skip of the same case, and never a real result."""
    for suite in ET.parse(later).getroot().iter("testsuite"):
        merged = suites.setdefault(suite.get("name", ""), ET.Element("testsuite", name=suite.get("name", "")))
        existing = {c.get("name"): c for c in merged.findall("testcase")}
        for case in suite.findall("testcase"):
            before = existing.get(case.get("name"))
            if before is None:
                merged.append(case)
            elif is_skipped(before) and not is_skipped(case):
                merged.remove(before)
                merged.append(case)


def merge(ctest_junits: list[Path], gtest_dir: Path, later: list[Path] | None = None) -> ET.Element:
    suites: dict[str, ET.Element] = {}  # gtest suite name -> merged <testsuite>
    kept = ET.Element("testsuite", name="ctest")
    for entry in [e for junit in ctest_junits for e in ET.parse(junit).getroot().iter("testcase")]:
        report = gtest_dir / (c_identifier(entry.get("name", "")) + ".xml")
        if report.is_file():
            for suite in ET.parse(report).getroot().iter("testsuite"):
                merged = suites.setdefault(suite.get("name", ""), ET.Element("testsuite", name=suite.get("name", "")))
                merged.extend(suite.findall("testcase"))
            if passed(entry):
                continue
        kept.append(entry)

    for report in later or []:
        if report.is_file():
            apply_later(suites, report)

    root = ET.Element("testsuites", name="AllTests")
    for suite in [*sorted(suites.values(), key=lambda s: s.get("name", "")), kept]:
        cases = suite.findall("testcase")
        if not cases:
            continue
        failures = sum(1 for c in cases if c.find("failure") is not None)
        errors = sum(1 for c in cases if c.find("error") is not None)
        skipped = sum(1 for c in cases if is_skipped(c))
        time = sum(float(c.get("time", "0") or 0) for c in cases)
        suite.set("tests", str(len(cases)))
        suite.set("failures", str(failures))
        suite.set("errors", str(errors))
        suite.set("skipped", str(skipped))
        suite.set("time", f"{time:.3f}")
        root.append(suite)
    for attr in ("tests", "failures", "errors", "skipped"):
        root.set(attr, str(sum(int(s.get(attr, "0")) for s in root)))
    root.set("time", f"{sum(float(s.get('time', '0')) for s in root):.3f}")
    return root


def main(argv: list[str]) -> int:
    args, later = [], []
    rest = iter(argv[1:])
    for arg in rest:
        if arg == "--later":
            later.append(Path(next(rest, "")))
        else:
            args.append(arg)
    if len(args) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    ctest_junits, gtest_dir, output = [Path(a) for a in args[:-2]], Path(args[-2]), Path(args[-1])
    found = [junit for junit in ctest_junits if junit.is_file()]
    if not found:
        print(f"merge-gtest-results: {', '.join(map(str, ctest_junits))} not found; nothing to merge", file=sys.stderr)
        return 0
    root = merge(found, gtest_dir, later)
    ET.indent(root)
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)
    print(
        f"merge-gtest-results: {root.get('tests')} testcases ({root.get('failures')} failed, "
        f"{root.get('errors')} errors, {root.get('skipped')} skipped) -> {output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
