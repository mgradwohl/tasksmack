#!/usr/bin/env python3
"""TaskSmack.exe enables SeDebugPrivilege at startup, before anything opens a process (#2128, #2478).

Platform::enableDebugPrivilege() is tested through injected token calls
(tests/Platform/test_WindowsDebugPrivilege.cpp), but its one caller is src/main.cpp, which no test
binary links: deleting the call, or moving it after the Application (and so the probes) exists,
failed no test. Calling the real function from a test would change the test process's own token, so
this reads main.cpp instead.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "src" / "main.cpp"

CALL = re.compile(r"\bPlatform::enableDebugPrivilege\s*\(\s*\)\s*;")
APPLICATION = re.compile(r"std::make_unique<\s*Core::Application\s*>")


def code_lines(text: str) -> list[str]:
    """`text`'s lines with // and /* */ comments blanked, so a call in a comment doesn't count."""
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group().count("\n"), text, flags=re.DOTALL)
    return [re.sub(r"//.*", "", line) for line in text.splitlines()]


def preprocessor_branch(lines: list[str], index: int) -> list[str]:
    """The #if/#ifdef conditions enclosing line `index`, outermost first."""
    stack: list[str] = []
    for line in lines[:index]:
        directive = line.strip()
        if re.match(r"#\s*if", directive):
            stack.append(directive)
        elif re.match(r"#\s*endif", directive) and stack:
            stack.pop()
        elif re.match(r"#\s*(else|elif)", directive) and stack:
            stack[-1] = directive
    return stack


class StartupWiringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.lines = code_lines(MAIN.read_text(encoding="utf-8"))

    def indices(self, pattern: re.Pattern) -> list[int]:
        return [index for index, line in enumerate(self.lines) if pattern.search(line)]

    def test_main_enables_the_debug_privilege_once(self) -> None:
        self.assertEqual(len(self.indices(CALL)), 1, "src/main.cpp must call Platform::enableDebugPrivilege() once")

    def test_it_is_enabled_before_the_application_and_its_probes_exist(self) -> None:
        calls = self.indices(CALL)
        applications = self.indices(APPLICATION)
        self.assertTrue(calls and applications)
        self.assertLess(calls[0], applications[0])

    def test_it_is_the_windows_build_that_calls_it(self) -> None:
        calls = self.indices(CALL)
        self.assertTrue(calls)
        branch = preprocessor_branch(self.lines, calls[0])
        self.assertEqual([re.sub(r"\s+", " ", directive) for directive in branch], ["#ifdef _WIN32"])


class CodeLinesTest(unittest.TestCase):
    def test_comments_are_blanked(self) -> None:
        lines = code_lines("a(); // Platform::enableDebugPrivilege();\n/* Platform::enableDebugPrivilege();\n*/ b();\n")
        self.assertEqual(len(lines), 3)
        self.assertFalse(any(CALL.search(line) for line in lines))
        self.assertIn("b();", lines[2])


if __name__ == "__main__":
    unittest.main()
