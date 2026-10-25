#!/usr/bin/env python3
"""Tests for tools/merge-gtest-results.py: ctest's per-suite JUnit report and gtest's per-case
reports merged into one report with a testcase per case (#1871)."""

import importlib.util
import shutil
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "merge-gtest-results.py"
spec = importlib.util.spec_from_file_location("merge_gtest_results", SCRIPT)
merge_gtest_results = importlib.util.module_from_spec(spec)
spec.loader.exec_module(merge_gtest_results)


def gtest_report(suite: str, cases: dict[str, str]) -> str:
    """A gtest XML report: each case "ok", "fail" or "skip"."""
    body = ""
    for name, outcome in cases.items():
        inner = {"ok": "", "fail": '<failure message="boom"/>', "skip": '<skipped message="no GPU"/>'}[outcome]
        body += f'<testcase name="{name}" status="run" time="0.5" classname="{suite}">{inner}</testcase>'
    return f'<testsuites name="AllTests"><testsuite name="{suite}">{body}</testsuite></testsuites>'


def ctest_entry(name: str, status: str = "run") -> str:
    failure = '<failure message="Failed"/>' if status == "fail" else ""
    return f'<testcase name="{name}" classname="{name}" time="1.0" status="{status}">{failure}</testcase>'


class MergeGtestResultsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="merge-gtest-results-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.gtest_dir = self.root / "gtest-results"
        self.gtest_dir.mkdir()

    def ctest(self, *entries: str) -> Path:
        path = self.root / "test-results.xml"
        path.write_text(f'<testsuite name="(empty)">{"".join(entries)}</testsuite>', encoding="utf-8")
        return path

    def report(self, entry: str, text: str) -> None:
        (self.gtest_dir / (merge_gtest_results.c_identifier(entry) + ".xml")).write_text(text, encoding="utf-8")

    def merged(self, ctest: Path) -> dict[str, ET.Element]:
        root = merge_gtest_results.merge([ctest], self.gtest_dir)
        return {s.get("name"): s for s in root.iter("testsuite")}

    def test_a_passing_suite_entry_is_replaced_by_its_cases(self) -> None:
        self.report("FormatTest", gtest_report("FormatTest", {"Bytes": "ok", "Rates": "skip"}))
        suites = self.merged(self.ctest(ctest_entry("FormatTest")))
        self.assertEqual(["FormatTest"], list(suites))
        self.assertEqual(["Bytes", "Rates"], [c.get("name") for c in suites["FormatTest"].iter("testcase")])
        self.assertEqual("2", suites["FormatTest"].get("tests"))
        self.assertEqual("1", suites["FormatTest"].get("skipped"))

    def test_shards_merge_into_one_suite(self) -> None:
        self.report("AppTest/shard1of2", gtest_report("AppTest", {"A": "ok"}))
        self.report("AppTest/shard2of2", gtest_report("AppTest", {"B": "ok"}))
        suites = self.merged(self.ctest(ctest_entry("AppTest/shard1of2"), ctest_entry("AppTest/shard2of2")))
        self.assertEqual(["AppTest"], list(suites))
        self.assertEqual("2", suites["AppTest"].get("tests"))

    def test_a_failing_entry_keeps_both_its_cases_and_its_ctest_result(self) -> None:
        self.report("ModelTest", gtest_report("ModelTest", {"Good": "ok", "Bad": "fail"}))
        suites = self.merged(self.ctest(ctest_entry("ModelTest", "fail")))
        self.assertEqual("1", suites["ModelTest"].get("failures"))
        self.assertEqual(["ModelTest"], [c.get("name") for c in suites["ctest"].iter("testcase")])
        self.assertEqual("1", suites["ctest"].get("failures"))

    def test_entries_without_a_report_are_kept(self) -> None:
        # A script test, and a suite whose process crashed before gtest wrote its report.
        suites = self.merged(self.ctest(ctest_entry("BenchScript"), ctest_entry("CrashTest", "fail")))
        self.assertEqual(["ctest"], list(suites))
        self.assertEqual(["BenchScript", "CrashTest"], [c.get("name") for c in suites["ctest"].iter("testcase")])

    def test_reports_of_entries_ctest_did_not_run_are_ignored(self) -> None:
        self.report("StaleTest", gtest_report("StaleTest", {"Old": "fail"}))
        self.report("FormatTest", gtest_report("FormatTest", {"Bytes": "ok"}))
        self.assertEqual(["FormatTest"], list(self.merged(self.ctest(ctest_entry("FormatTest")))))

    def test_totals_add_up_on_the_root(self) -> None:
        self.report("ModelTest", gtest_report("ModelTest", {"Good": "ok", "Bad": "fail"}))
        root = merge_gtest_results.merge([self.ctest(ctest_entry("ModelTest", "fail"), ctest_entry("BenchScript"))], self.gtest_dir)
        self.assertEqual(("4", "2"), (root.get("tests"), root.get("failures")))

    def test_main_writes_the_report_and_tolerates_a_missing_ctest_report(self) -> None:
        output = self.root / "merged.xml"
        self.assertEqual(0, merge_gtest_results.main(["x", str(self.root / "missing.xml"), str(self.gtest_dir), str(output)]))
        self.assertFalse(output.exists())
        self.report("FormatTest", gtest_report("FormatTest", {"Bytes": "ok"}))
        ctest = self.ctest(ctest_entry("FormatTest"))
        self.assertEqual(0, merge_gtest_results.main(["x", str(ctest), str(self.gtest_dir), str(output)]))
        self.assertEqual("1", ET.parse(output).getroot().get("tests"))

    def test_several_ctest_reports_merge_as_one_and_a_missing_one_is_skipped(self) -> None:
        """#2832: Windows ARM64 runs the pwsh script tests in a second ctest run, with its own report."""
        output = self.root / "merged.xml"
        self.report("FormatTest", gtest_report("FormatTest", {"Bytes": "ok", "Rate": "ok"}))
        suites = self.ctest(ctest_entry("FormatTest"))
        scripts = self.root / "test-results-pwsh-script.xml"
        scripts.write_text(f'<testsuite name="ctest">{ctest_entry("EtwCaptureScript")}{ctest_entry("SetupDevScript", "fail")}</testsuite>', encoding="utf-8")
        argv = ["x", str(suites), str(scripts), str(self.root / "missing.xml"), str(self.gtest_dir), str(output)]
        self.assertEqual(0, merge_gtest_results.main(argv))
        root = ET.parse(output).getroot()
        self.assertEqual(("4", "1"), (root.get("tests"), root.get("failures")))
        self.assertEqual({"FormatTest", "ctest"}, {suite.get("name") for suite in root})
        self.assertEqual(2, len(root.find("testsuite[@name='ctest']").findall("testcase")))

    def test_c_identifier_matches_cmake(self) -> None:
        self.assertEqual("Each_Suite_shard1of2", merge_gtest_results.c_identifier("Each/Suite/shard1of2"))
        self.assertEqual("Suite_Case", merge_gtest_results.c_identifier("Suite.Case"))
        self.assertEqual("_1Suite", merge_gtest_results.c_identifier("1Suite"))


if __name__ == "__main__":
    unittest.main()
