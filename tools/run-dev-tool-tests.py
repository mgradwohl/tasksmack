#!/usr/bin/env python3
"""
tools/run-dev-tool-tests.py -- Run the tool tests ci.yml's dev-tools-checks jobs stand in for (#4248).

Usage:
    tools/run-dev-tool-tests.py linux|windows

A pull request that changes only developer tools (tools/ci-change-classifier.py's dev_tools_only) skips the
C++ jobs, and with them ctest, which runs these tests. This runs the same ones, the way their ctest entries
in tests/CMakeLists.txt do: each test file named in the classifier's DEV_TOOL_TESTS for the runner, one at a
time (the pwsh ones take turns there too, RESOURCE_LOCK pwsh-script-tests), a Python test with
TASKSMACK_TEST_BASH set to bash, a pwsh one with TASKSMACK_TEST_PYTHON set to this Python. Each gets the
slowest ctest TIMEOUT any of them has (600 s). Exits 1 if any fails or times out, 2 on bad usage.

Changing this script gives every job (FULL_VALIDATION), so a change here can't skip what would test it.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TIMEOUT_SECONDS = 600


def dev_tool_tests() -> dict[str, tuple[str, ...]]:
    spec = importlib.util.spec_from_file_location("ci_change_classifier", REPO / "tools" / "ci-change-classifier.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["ci_change_classifier"] = module  # dataclasses looks the module up by name
    spec.loader.exec_module(module)
    return module.DEV_TOOL_TESTS


def command(test: str) -> tuple[list[str], dict[str, str]]:
    """How ctest runs `test`, and the environment it adds."""
    if test.endswith(".ps1"):
        pwsh = shutil.which("pwsh") or "pwsh"
        return [pwsh, "-NoProfile", "-File", str(REPO / test)], {"TASKSMACK_TEST_PYTHON": sys.executable}
    bash = shutil.which("bash") or "bash"
    return [sys.executable, "-X", "utf8", str(REPO / test)], {"TASKSMACK_TEST_BASH": bash}


def main(argv: list[str]) -> int:
    tests = dev_tool_tests()
    if len(argv) != 1 or argv[0] not in tests:
        print(f"usage: run-dev-tool-tests.py {'|'.join(tests)}", file=sys.stderr)
        return 2
    failed: list[str] = []
    for test in tests[argv[0]]:
        args, extra = command(test)
        print(f"::group::{test}", flush=True)
        try:
            result = subprocess.run(args, cwd=REPO, env={**os.environ, **extra}, check=False, timeout=TIMEOUT_SECONDS)
            code = result.returncode
        except subprocess.TimeoutExpired:
            print(f"{test}: timed out after {TIMEOUT_SECONDS} s", flush=True)
            code = -1
        print("::endgroup::", flush=True)
        if code != 0:
            print(f"::error title=Developer tool test::{test} failed (exit {code})", flush=True)
            failed.append(test)
    if failed:
        print(f"{len(failed)} of {len(tests[argv[0]])} failed: {', '.join(failed)}")
        return 1
    print(f"All {len(tests[argv[0]])} {argv[0]} developer tool tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
