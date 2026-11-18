#!/usr/bin/env python3
"""Tests that the committed perf-data/ baselines match the BENCHMARK(...) registrations (#4207).

benchmarks/CMakeLists.txt says which sources each platform builds; the registrations in them are what
Google Benchmark reports, and perf-data/win-baseline.json and perf-data/linux-ci-baseline.json are captures
of those reports. A baseline that names a benchmark nobody registers any more is stale (the regression gate
would never compare it), and a registered benchmark the baseline lacks is never gated. So, per platform:

- No baseline name may be unregistered (a removed or renamed benchmark).
- At least 90% of the registered names must be in the baseline.

Names are compared by family: the text before the first "/", so a benchmark's arguments (->Arg, ->Args,
->ArgsProduct, ->Apply, often constants from the source) don't matter, only the name Google Benchmark starts
from: BENCHMARK(fn) is "fn", ->Name("x") is "x", BENCHMARK_CAPTURE(fn, case, ...) is "fn/case" (family "fn"),
BENCHMARK_TEMPLATE(fn, T) is "fn<T>". The parser has its own unit tests on small fixtures below.
"""

from __future__ import annotations

import json
import re
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BENCHMARKS = REPO / "benchmarks"
PERF_DATA = REPO / "perf-data"
MIN_COVERAGE = 0.90

# The committed baselines per platform. Each is held to the same rule.
BASELINES = {"windows": ["win-baseline.json"], "linux": ["linux-ci-baseline.json"]}

MACRO = re.compile(r"\b(BENCHMARK(?:_CAPTURE|_TEMPLATE\d*)?)\s*\(")
NAME_OPTION = re.compile(r'->\s*Name\s*\(\s*"((?:[^"\\]|\\.)*)"\s*\)')


def blank_comments(source: str, blank_strings: bool = False) -> str:
    """The source with // and /* */ comments (and, if asked, string literals' contents) blanked to spaces, so
    every index still points at the same place in the original."""
    out = list(source)
    i, n = 0, len(source)

    def blank(start: int, end: int) -> None:
        for k in range(start, min(end, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        two = source[i : i + 2]
        if two == "//":
            end = source.find("\n", i)
            end = n if end < 0 else end
            blank(i, end)
            i = end
        elif two == "/*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif source[i] == '"':
            j = i + 1
            while j < n and source[j] != '"':
                j += 2 if source[j] == "\\" else 1
            if blank_strings:
                blank(i + 1, j)
            i = j + 1
        else:
            i += 1
    return "".join(out)


def split_arguments(text: str) -> tuple[list[str], int]:
    """The top-level comma-separated arguments of a call whose "(" was just consumed, and the index after
    its ")". Nested (), {}, [] and string literals are respected; "<" is not (it is also less-than)."""
    arguments: list[str] = []
    depth, start, i = 0, 0, 0
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            if depth == 0:
                arguments.append(text[start:i].strip())
                return arguments, i + 1
            depth -= 1
        elif c == "," and depth == 0:
            arguments.append(text[start:i].strip())
            start = i + 1
        i += 1
    raise ValueError("unbalanced parentheses in a BENCHMARK registration")


PLATFORM_MACROS = {"windows": {"_WIN32", "WIN32"}, "linux": {"__linux__", "__unix__"}}
DIRECTIVE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")


def _condition(directive: str, expression: str, macros: set[str]) -> bool | None:
    """Whether a conditional directive's condition holds when `macros` are the only ones defined; None when
    it is not a plain combination of defined()/ifdef checks, 0, 1, !, && and ||."""
    expression = expression.split("//")[0].strip()
    if directive in ("ifdef", "ifndef"):
        value = expression in macros
        return value if directive == "ifdef" else not value
    expression = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", lambda m: "1" if (m.group(1) or m.group(2)) in macros else "0", expression)
    if not re.fullmatch(r"[01!&| ()\t]*", expression):
        return None
    try:
        return bool(eval(expression.replace("!", " not ").replace("&&", " and ").replace("||", " or "), {"__builtins__": {}}))  # noqa: S307
    except SyntaxError:
        return None


def blank_line(line: str) -> str:
    return "".join(c if c == "\n" else " " for c in line)


def for_platform(source: str, platform: str) -> str:
    """The source with the lines the platform's preprocessor drops (#if/#ifdef/#else of _WIN32 and
    __linux__) blanked. A condition this can't evaluate keeps all its branches."""
    macros = PLATFORM_MACROS[platform]
    out: list[str] = []
    # One frame per open #if: whether the enclosing code is live, whether a branch was taken, whether this one is live.
    stack: list[list[bool]] = []
    live = True
    for line in source.splitlines(keepends=True):
        match = DIRECTIVE.match(line)
        if match:
            directive, rest = match.groups()
            if directive in ("if", "ifdef", "ifndef"):
                value = _condition(directive, rest, macros)
                stack.append([live, value is True, value is not False])
                live = live and value is not False
            elif directive == "elif" and stack:
                outer, taken, _ = stack[-1]
                value = _condition("if", rest, macros)
                stack[-1] = [outer, taken or value is True, not taken and value is not False]
                live = outer and stack[-1][2]
            elif directive == "else" and stack:
                outer, taken, _ = stack[-1]
                stack[-1][2] = not taken
                live = outer and not taken
            elif directive == "endif" and stack:
                live = stack.pop()[0]
            out.append(line)
        else:
            out.append(line if live else blank_line(line))
    return "".join(out)


def registered_names(source: str, platform: str | None = None) -> list[str]:
    """The names Google Benchmark reports for each BENCHMARK registration in `source`, before arguments
    are appended: "fn", the ->Name override, "fn/case" or "fn<T>"."""
    if platform:
        source = for_platform(blank_comments(source), platform)
    text = blank_comments(source)
    masked = blank_comments(source, blank_strings=True)  # a macro inside a string is not a registration
    names: list[str] = []
    for match in MACRO.finditer(masked):
        macro = match.group(1)
        arguments, consumed = split_arguments(text[match.end() :])
        function = arguments[0]
        if macro == "BENCHMARK_CAPTURE":
            base = f"{function}/{arguments[1]}"
        elif macro.startswith("BENCHMARK_TEMPLATE"):
            base = f"{function}<{', '.join(arguments[1:])}>"
        else:
            base = function
        # The chain runs to the statement's ";". A later ->Name replaces the name, as the library does.
        start = match.end() + consumed
        end = text.find(";", start)
        chain = text[start : end if end >= 0 else len(text)]
        overrides = NAME_OPTION.findall(chain)
        names.append(overrides[-1] if overrides else base)
    return names


def family(name: str) -> str:
    """A reported name's family: the text before the first "/" outside template angle brackets."""
    depth = 0
    for i, c in enumerate(name):
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == "/" and depth == 0:
            return name[:i]
    return name


def _platform_list(body: str) -> list[str]:
    block = re.search(r"set\s*\(\s*PLATFORM_BENCH_SOURCES(.*?)\)", body, re.S)
    if not block:
        raise ValueError("no set(PLATFORM_BENCH_SOURCES ...) in the benchmarks' CMakeLists.txt")
    return re.findall(r"[\w./-]+\.cpp", block.group(1))


def platform_sources(cmake: str) -> dict[str, list[str]]:
    """The .cpp files each platform's TaskSmackBenchmarks builds, read from benchmarks/CMakeLists.txt: the
    add_executable list, with ${PLATFORM_BENCH_SOURCES} replaced by the if(WIN32) set() for Windows and by
    the else() one for Linux."""
    text = re.sub(r"#[^\n]*", "", cmake)
    block = re.search(r"if\s*\(\s*WIN32\s*\)(.*?)\belse\s*\(\s*\)(.*?)\bendif\s*\(\s*\)", text, re.S)
    if not block:
        raise ValueError("no if(WIN32) ... else() ... endif() in the benchmarks' CMakeLists.txt")
    executable = re.search(r"add_executable\s*\(\s*TaskSmackBenchmarks(.*?)\n\s*\)", text, re.S)
    if not executable:
        raise ValueError("no add_executable(TaskSmackBenchmarks ...) in the benchmarks' CMakeLists.txt")
    common = re.findall(r"^\s*([\w./-]+\.cpp)\s*$", executable.group(1), re.M)
    return {"windows": common + _platform_list(block.group(1)), "linux": common + _platform_list(block.group(2))}


def baseline_families(path: Path) -> set[str]:
    """The families a baseline's entries come from: the entry's run_name (an aggregate's name has a suffix)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return {family(entry.get("run_name") or entry["name"]) for entry in data["benchmarks"]}


def registered_families(sources: list[str], platform: str | None = None) -> set[str]:
    families: set[str] = set()
    for source in sources:
        families.update(family(name) for name in registered_names((BENCHMARKS / source).read_text(encoding="utf-8"), platform))
    return families


class ParserTest(unittest.TestCase):
    def test_plain_chain_args_and_units_are_the_function_name(self) -> None:
        source = "BENCHMARK(BM_A);\nBENCHMARK(BM_B)->Arg(1)->Arg(2)->Unit(benchmark::kMicrosecond);\n"
        self.assertEqual(registered_names(source), ["BM_A", "BM_B"])

    def test_a_chain_over_several_lines_with_nested_calls(self) -> None:
        source = "BENCHMARK(BM_A)\n    ->ArgsProduct({{300, 3000}, {1, 10}, {K, J}})\n    ->Unit(benchmark::kMillisecond);\nBENCHMARK(BM_B);\n"
        self.assertEqual(registered_names(source), ["BM_A", "BM_B"])

    def test_the_name_option_replaces_the_name(self) -> None:
        self.assertEqual(registered_names('BENCHMARK(BM_A)->Name("Renamed")->Arg(3);'), ["Renamed"])

    def test_the_platforms_preprocessor_drops_the_other_platforms_registrations(self) -> None:
        source = "BENCHMARK(BM_All);\n#ifdef _WIN32\nBENCHMARK(BM_Win);\n#else\nBENCHMARK(BM_Posix);\n#endif\nBENCHMARK(BM_Tail);\n"
        self.assertEqual(registered_names(source, "windows"), ["BM_All", "BM_Win", "BM_Tail"])
        self.assertEqual(registered_names(source, "linux"), ["BM_All", "BM_Posix", "BM_Tail"])
        self.assertEqual(registered_names(source), ["BM_All", "BM_Win", "BM_Posix", "BM_Tail"])

    def test_if_defined_elif_and_nesting(self) -> None:
        source = (
            "#if defined(_WIN32) && !defined(X)\nBENCHMARK(BM_A);\n#elif defined(__linux__)\nBENCHMARK(BM_B);\n"
            "#ifndef _WIN32\nBENCHMARK(BM_C);\n#endif\n#else\nBENCHMARK(BM_D);\n#endif\n"
        )
        self.assertEqual(registered_names(source, "windows"), ["BM_A"])
        self.assertEqual(registered_names(source, "linux"), ["BM_B", "BM_C"])

    def test_a_condition_it_cannot_evaluate_keeps_every_branch(self) -> None:
        source = "#if FEATURE_LEVEL > 2\nBENCHMARK(BM_A);\n#else\nBENCHMARK(BM_B);\n#endif\n"
        self.assertEqual(registered_names(source, "linux"), ["BM_A", "BM_B"])

    def test_the_name_option_does_not_leak_into_the_next_registration(self) -> None:
        self.assertEqual(registered_names('BENCHMARK(BM_A)->Name("R");\nBENCHMARK(BM_B);'), ["R", "BM_B"])

    def test_capture_is_function_slash_case(self) -> None:
        names = registered_names("BENCHMARK_CAPTURE(BM_Sort, small, 10, 20)->Arg(1);")
        self.assertEqual(names, ["BM_Sort/small"])
        self.assertEqual(family(names[0]), "BM_Sort")

    def test_templates_keep_their_arguments(self) -> None:
        source = "BENCHMARK_TEMPLATE(BM_Fill, int)->Arg(8);\nBENCHMARK_TEMPLATE2(BM_Map, int, std::string);"
        self.assertEqual(registered_names(source), ["BM_Fill<int>", "BM_Map<int, std::string>"])
        self.assertEqual(family("BM_Fill<int>/8"), "BM_Fill<int>")

    def test_a_template_argument_with_a_slash_stays_in_the_family(self) -> None:
        self.assertEqual(family("BM_X<std::ratio<1/2>>/64"), "BM_X<std::ratio<1/2>>")

    def test_comments_and_strings_are_not_registrations(self) -> None:
        source = '// BENCHMARK(BM_Gone);\n/* BENCHMARK(BM_Gone2); */\nconst char* s = "BENCHMARK(BM_Str)"; // x\nBENCHMARK(BM_Kept);\n'
        self.assertEqual(registered_names(source), ["BM_Kept"])

    def test_a_benchmark_registered_twice_is_listed_twice_and_one_family(self) -> None:
        source = "BENCHMARK(BM_A)->Iterations(1);\nBENCHMARK(BM_A)->Iterations(10);\n"
        self.assertEqual(registered_names(source), ["BM_A", "BM_A"])

    def test_benchmark_main_and_other_macros_are_ignored(self) -> None:
        self.assertEqual(registered_names("BENCHMARK_MAIN();\nBENCHMARK_DEFINE_F(Fx, Foo)(auto& s) {}\n"), [])

    def test_an_unbalanced_registration_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            registered_names("BENCHMARK(BM_A")

    def test_family_of_reported_names(self) -> None:
        self.assertEqual(family("BM_A"), "BM_A")
        self.assertEqual(family("BM_A/64"), "BM_A")
        self.assertEqual(family("BM_A/disks:16/iterations:10"), "BM_A")

    def test_platform_sources_come_from_the_cmake_lists(self) -> None:
        cmake = """
        if(WIN32)
            set(PLATFORM_BENCH_SOURCES
                bench_Win.cpp
            )
        else()
            set(PLATFORM_BENCH_SOURCES
                # Linux-specific
                bench_Lin.cpp
                bench_Lin2.cpp
            )
        endif()
        add_executable(TaskSmackBenchmarks
            bench_main.cpp
            bench_Common.cpp
            ${PLATFORM_BENCH_SOURCES}
            $<TARGET_OBJECTS:TaskSmackApp>
        )
        """
        self.assertEqual(
            platform_sources(cmake),
            {
                "windows": ["bench_main.cpp", "bench_Common.cpp", "bench_Win.cpp"],
                "linux": ["bench_main.cpp", "bench_Common.cpp", "bench_Lin.cpp", "bench_Lin2.cpp"],
            },
        )

    def test_baseline_families_use_the_run_name_not_the_aggregate_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.json"
            path.write_text(
                json.dumps(
                    {
                        "benchmarks": [
                            {"name": "BM_A/8", "run_name": "BM_A/8"},
                            {"name": "BM_A/8_mean", "run_name": "BM_A/8", "aggregate_name": "mean"},
                            {"name": "BM_B_cv", "run_name": "BM_B", "aggregate_name": "cv"},
                        ]
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(baseline_families(path), {"BM_A", "BM_B"})


class BaselinesMatchRegistrationsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cmake = (BENCHMARKS / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.sources = platform_sources(cmake)

    def test_every_listed_source_exists_and_registers_something(self) -> None:
        for platform, sources in self.sources.items():
            for source in sources:
                with self.subTest(f"{platform}: {source}"):
                    self.assertTrue((BENCHMARKS / source).is_file())
            with self.subTest(platform):
                self.assertTrue(registered_families(sources, platform))

    def check(self, platform: str, baseline: str) -> None:
        registered = registered_families(self.sources[platform], platform)
        baselined = baseline_families(PERF_DATA / baseline)
        removed = sorted(baselined - registered)
        self.assertFalse(removed, f"perf-data/{baseline} holds benchmarks no {platform} source registers: {removed}")
        coverage = len(baselined & registered) / len(registered)
        missing = sorted(registered - baselined)
        self.assertGreaterEqual(
            coverage, MIN_COVERAGE, f"perf-data/{baseline} covers {coverage:.0%} of the {platform} benchmarks; missing: {missing}"
        )

    def test_windows_baseline_matches_the_windows_registrations(self) -> None:
        for baseline in BASELINES["windows"]:
            with self.subTest(baseline):
                self.check("windows", baseline)

    def test_linux_baseline_matches_the_linux_registrations(self) -> None:
        for baseline in BASELINES["linux"]:
            with self.subTest(baseline):
                self.check("linux", baseline)


if __name__ == "__main__":
    unittest.main()
