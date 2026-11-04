#!/usr/bin/env python3
"""Tests for tools/allocation-hook-summary.py: which tests ran with the allocation-failure hook armed, and
--require failing a build where the hook is compiled in but no test armed it or one skipped for want of it (#3342)."""

import contextlib
import importlib.util
import io
import shutil
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "allocation-hook-summary.py"
spec = importlib.util.spec_from_file_location("allocation_hook_summary", SCRIPT)
allocation_hook_summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(allocation_hook_summary)

SANITIZER_SKIP = "tests/Domain/test_GPUModelAllocationFailure.cpp:62\nthe sanitizer runtime owns operator new"


def cases_report(*cases: tuple[str, str, str]) -> str:
    """A merged per-case report: each case (suite, name, outcome), outcome "ok", "hook-skip" or "other-skip"."""
    body = ""
    for suite, name, outcome in cases:
        inner = {
            "ok": "",
            "hook-skip": f'<skipped message="{SANITIZER_SKIP}"/>',
            "other-skip": '<skipped message="No GPU here"/>',
        }[outcome]
        body += f'<testcase name="{name}" classname="{suite}" status="run">{inner}</testcase>'
    return f'<testsuites><testsuite name="all">{body}</testsuite></testsuites>'


class AllocationHookSummaryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="allocation-hook-summary-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.log = self.root / "allocation-hook.log"
        self.cases = self.root / "test-results-cases.xml"

    def run_main(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = allocation_hook_summary.main([*args, str(self.log), str(self.cases)])
        return code, out.getvalue()

    def test_armed_cases_are_counted_once_each_by_suite(self) -> None:
        # A sweep arms the hook many times; the hook writes each test once, but a repeat line counts once too.
        self.log.write_text("GPUModelAllocationFailureTest.A\nGPUModelAllocationFailureTest.A\nHandleSearchTest.B\n\n")
        self.cases.write_text(cases_report(("GPUModelAllocationFailureTest", "A", "ok"), ("HandleSearchTest", "B", "ok")))
        code, out = self.run_main("--require")
        self.assertEqual(0, code)
        self.assertIn("armed in 2 test case(s) across 2 suite(s); 0 case(s) skipped", out)
        self.assertIn("- `GPUModelAllocationFailureTest.A`", out)
        self.assertNotIn("::error", out)

    def test_a_case_skipped_without_the_hook_fails_only_under_require(self) -> None:
        self.log.write_text("HandleSearchTest.B\n")
        self.cases.write_text(
            cases_report(
                ("GPUModelAllocationFailureTest", "A", "hook-skip"),
                ("HandleSearchTest", "B", "ok"),
                ("GPUProbeTest", "C", "other-skip"),  # skipped for another reason: not the hook's
            )
        )
        code, out = self.run_main("--require")
        self.assertEqual(1, code)
        self.assertIn("1 case(s) skipped because the hook is compiled out", out)
        self.assertIn("- `GPUModelAllocationFailureTest.A`", out)
        self.assertNotIn("GPUProbeTest.C", out)
        self.assertIn("::error title=allocation-failure hook::1 test case(s) skipped", out)
        code, out = self.run_main()
        self.assertEqual(0, code)
        self.assertNotIn("::error", out)

    def test_no_armed_case_fails_under_require(self) -> None:
        # No log at all: the hook never armed (or its logging broke), which a build with the hook can't pass.
        self.cases.write_text(cases_report(("SomeTest", "A", "ok")))
        code, out = self.run_main("--require")
        self.assertEqual(1, code)
        self.assertIn("armed in 0 test case(s) across 0 suite(s)", out)
        self.assertIn("::error title=allocation-failure hook::No test armed", out)
        self.assertEqual(0, self.run_main()[0])

    def test_a_missing_report_prints_nothing_and_passes(self) -> None:
        # The build failed before the tests ran: the build's own failure is the one to read.
        self.assertEqual((0, ""), self.run_main("--require"))

    def test_a_wrong_argument_count_is_a_usage_error(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, allocation_hook_summary.main(["only-one"]))


if __name__ == "__main__":
    unittest.main()
