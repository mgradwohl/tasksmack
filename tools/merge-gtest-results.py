#!/usr/bin/env python3
"""
tools/merge-gtest-results.py -- One JUnit report with a testcase per Google Test case (#1871).

Usage:
    merge-gtest-results.py <ctest-junit.xml>... <gtest-results-dir> <output.xml>

ctest runs one entry per gtest suite (cmake/GTestSuites.cmake), so its --output-junit report has a
testcase per suite. Each entry also writes gtest's own XML report, one testcase per case, to
<gtest-results-dir>/<entry name as a C identifier>.xml. This merges the two: every gtest case from
the reports of the entries ctest ran, plus the ctest entries that have no gtest report (the script
tests) or did not pass (a crash or timeout leaves no report, and a failing entry's output names
the cases it ran). Shards of one suite are merged back into one testsuite.

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


def merge(ctest_junits: list[Path], gtest_dir: Path) -> ET.Element:
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

    root = ET.Element("testsuites", name="AllTests")
    for suite in [*sorted(suites.values(), key=lambda s: s.get("name", "")), kept]:
        cases = suite.findall("testcase")
        if not cases:
            continue
        failures = sum(1 for c in cases if c.find("failure") is not None)
        errors = sum(1 for c in cases if c.find("error") is not None)
        skipped = sum(1 for c in cases if c.find("skipped") is not None or c.get("status") in ("notrun", "disabled"))
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
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    ctest_junits, gtest_dir, output = [Path(a) for a in argv[1:-2]], Path(argv[-2]), Path(argv[-1])
    found = [junit for junit in ctest_junits if junit.is_file()]
    if not found:
        print(f"merge-gtest-results: {', '.join(map(str, ctest_junits))} not found; nothing to merge", file=sys.stderr)
        return 0
    root = merge(found, gtest_dir)
    ET.indent(root)
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)
    print(
        f"merge-gtest-results: {root.get('tests')} testcases ({root.get('failures')} failed, "
        f"{root.get('errors')} errors, {root.get('skipped')} skipped) -> {output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
