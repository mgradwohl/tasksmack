#!/usr/bin/env python3
"""Tests for tools/cpp-deps-sbom.py (#4438): the source SBOM lists every C/C++ library the FetchContent pins
declare, at its pinned commit, with its licence; a declaration in a shape the script can't read, or in a CMake
file it doesn't read, fails instead of being left out; `check` fails a document missing a pin; and `osv` fails
on a vulnerable pinned commit unless osv-scanner.toml ignores it with a reason and a date. Never touches the
network: OSV is a fake.
"""

import datetime
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "cpp-deps-sbom.py"
spec = importlib.util.spec_from_file_location("cpp_deps_sbom", SCRIPT)
sbom = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sbom  # dataclasses looks its module up
spec.loader.exec_module(sbom)

SHA_A = "a" * 40
SHA_B = "b" * 40
SHA_T = "c" * 40

DEPENDENCIES = f"""# FetchContent_Declare() named in a comment doesn't count.
FetchContent_Declare(
    alpha
    GIT_REPOSITORY https://github.com/Owner/Alpha.git
    GIT_TAG {SHA_A}  # v1.2.3 - pinned to SHA for supply chain security
    SYSTEM  # (#1308)
    BINARY_DIR "${{TASKSMACK_DEPS_BINARY_DIR}}/alpha-build"  # per-preset build tree (#1308)
)
FetchContent_Declare(
    beta
    GIT_REPOSITORY https://github.com/owner/beta.git
    GIT_TAG {SHA_B}  # master after v2.0 - pinned to SHA for supply chain security
    PATCH_COMMAND "${{CMAKE_COMMAND}}" -P "x.cmake"
)
"""
NOTICES = """set(_TS_NOTICE_LICENSES
    "alpha|MIT"
    "beta|FTL OR GPL-2.0-or-later (used under the FTL)")
"""
TESTS = f"""FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG {SHA_T}  # v1.18.0 - pinned to SHA for supply chain security
)
"""
TOP = """pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)
pkg_check_modules(LIBCURL QUIET IMPORTED_TARGET libcurl)
"""
SYFT = {
    "spdxVersion": "SPDX-2.3",
    "SPDXID": "SPDXRef-DOCUMENT",
    "packages": [{"name": ".", "SPDXID": "SPDXRef-DocumentRoot-Directory-."},
                 {"name": "actions/checkout", "SPDXID": "SPDXRef-Package-github-action"}],
    "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                       "relatedSpdxElement": "SPDXRef-DocumentRoot-Directory-."}],
}


class Tree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.write("cmake/Dependencies.cmake", DEPENDENCIES)
        self.write("cmake/ThirdPartyNotices.cmake", NOTICES)
        self.write("tests/CMakeLists.txt", TESTS)
        self.write("CMakeLists.txt", TOP)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, relative: str, text: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = sbom.main([*args, "--root", str(self.root)])
        return code, out.getvalue(), err.getvalue()

    def merged(self) -> dict:
        return sbom.merge(json.loads(json.dumps(SYFT)), sbom.components(self.root))


class ComponentsTest(Tree):
    def test_every_pin_is_read_with_its_version_purl_licence_and_scope(self):
        by_name = {dependency.name: dependency for dependency in sbom.components(self.root)}
        self.assertEqual(list(by_name), ["alpha", "beta", "googletest", "libbpf", "libcurl"])
        alpha = by_name["alpha"]
        self.assertEqual((alpha.scope, alpha.version, alpha.license), ("shipped", "v1.2.3", "MIT"))
        self.assertEqual(alpha.purl, f"pkg:github/owner/alpha@{SHA_A}")
        self.assertEqual(alpha.download_location, f"git+https://github.com/Owner/Alpha.git@{SHA_A}")
        beta = by_name["beta"]
        # An untagged pin is versioned by its commit, and keeps its comment as a note.
        self.assertEqual((beta.version, beta.note), (SHA_B, "master after v2.0"))
        # A notice licence with prose becomes an SPDX expression, the prose kept as a comment.
        self.assertEqual(beta.license, "FTL OR GPL-2.0-or-later")
        self.assertEqual(beta.license_text, "FTL OR GPL-2.0-or-later (used under the FTL)")
        self.assertEqual((by_name["googletest"].scope, by_name["googletest"].license), ("test", "BSD-3-Clause"))
        self.assertEqual((by_name["libbpf"].scope, by_name["libbpf"].purl), ("system", ""))
        self.assertIn(">=1.0", by_name["libbpf"].note)

    def test_a_shipped_dependency_without_a_notice_licence_fails(self):
        self.write("cmake/ThirdPartyNotices.cmake", 'set(_TS_NOTICE_LICENSES\n    "alpha|MIT")\n')
        with self.assertRaisesRegex(sbom.DependencyError, "beta ships but has no licence"):
            sbom.components(self.root)

    def test_a_declaration_in_another_shape_fails(self):
        for body in ("FetchContent_Declare(\n    gamma\n    URL https://example.com/gamma.tar.gz\n    URL_HASH SHA256=00\n)\n",
                     "FetchContent_Declare(\n    gamma\n    GIT_REPOSITORY https://github.com/o/gamma.git\n    GIT_TAG v1.0\n)\n",
                     "FetchContent_Declare(gamma\n    GIT_REPOSITORY https://github.com/o/gamma.git\n    GIT_TAG " + SHA_A + "\n)\n"):
            with self.subTest(body=body.splitlines()[1]):
                self.write("cmake/Dependencies.cmake", DEPENDENCIES + body)
                with self.assertRaises(sbom.DependencyError):
                    sbom.components(self.root)

    def test_a_declaration_in_a_file_the_script_does_not_read_fails(self):
        self.write("src/extra/CMakeLists.txt", TESTS.replace("googletest", "sneaky"))
        with self.assertRaisesRegex(sbom.DependencyError, "src/extra/CMakeLists.txt calls FetchContent_Declare"):
            sbom.components(self.root)

    def test_build_trees_and_dot_directories_are_not_read(self):
        self.write("build/debug/_deps/x/CMakeLists.txt", TESTS)
        self.write(".claude/worktrees/w/cmake/x.cmake", TESTS)
        self.assertEqual(len(sbom.components(self.root)), 5)


class MergeAndCheckTest(Tree):
    def test_merge_adds_packages_and_relationships_and_keeps_syfts(self):
        document = self.merged()
        ids = [package["SPDXID"] for package in document["packages"]]
        self.assertIn("SPDXRef-Package-github-action", ids)
        alpha = next(package for package in document["packages"] if package["name"] == "alpha")
        self.assertEqual(alpha["SPDXID"], "SPDXRef-CppDep-alpha")
        self.assertEqual(alpha["versionInfo"], "v1.2.3")
        self.assertEqual(alpha["licenseDeclared"], "MIT")
        self.assertEqual(alpha["externalRefs"][0]["referenceLocator"], f"pkg:github/owner/alpha@{SHA_A}")
        root = "SPDXRef-DocumentRoot-Directory-."
        self.assertIn({"spdxElementId": root, "relationshipType": "STATIC_LINK",
                       "relatedSpdxElement": "SPDXRef-CppDep-alpha"}, document["relationships"])
        self.assertIn({"spdxElementId": "SPDXRef-CppDep-googletest", "relationshipType": "TEST_DEPENDENCY_OF",
                       "relatedSpdxElement": root}, document["relationships"])
        self.assertIn({"spdxElementId": root, "relationshipType": "DYNAMIC_LINK",
                       "relatedSpdxElement": "SPDXRef-CppDep-libbpf"}, document["relationships"])
        self.assertNotIn("externalRefs", next(p for p in document["packages"] if p["name"] == "libbpf"))

    def test_merge_is_idempotent(self):
        once = self.merged()
        twice = sbom.merge(json.loads(json.dumps(once)), sbom.components(self.root))
        self.assertEqual(once, twice)

    def test_check_passes_a_merged_document(self):
        self.assertEqual(sbom.check(self.merged(), sbom.components(self.root)), [])

    def test_check_fails_syfts_document_alone(self):
        problems = sbom.check(json.loads(json.dumps(SYFT)), sbom.components(self.root))
        self.assertEqual(len(problems), 5, problems)
        self.assertTrue(all("missing from the SBOM" in problem for problem in problems))

    def test_check_fails_a_stale_pin_and_a_dependency_no_longer_declared(self):
        document = self.merged()
        self.write("cmake/Dependencies.cmake", DEPENDENCIES.replace(SHA_A, "d" * 40).split("FetchContent_Declare(\n    beta")[0])
        problems = sbom.check(document, sbom.components(self.root))
        self.assertTrue(any("alpha isn't listed at its pinned commit " + "d" * 40 in p for p in problems), problems)
        self.assertTrue(any("lists beta" in p and "no CMake file declares" in p for p in problems), problems)

    def test_the_command_line_merges_in_place_then_checks(self):
        path = self.write("sbom.spdx.json", json.dumps(SYFT))
        code, _, err = self.run_main("check", "--sbom", str(path))
        self.assertEqual(code, 1)
        self.assertIn("alpha (cmake/Dependencies.cmake) is missing", err)
        code, out, _ = self.run_main("merge", "--sbom", str(path))
        self.assertEqual(code, 0)
        self.assertIn("Added 5 C/C++ components", out)
        code, out, err = self.run_main("check", "--sbom", str(path))
        self.assertEqual(code, 0, err)
        self.assertIn("lists all 5 C/C++ components", out)


class OsvTest(Tree):
    def fake_osv(self, vulnerable: dict[str, list[str]]):
        self.queries = []

        def post(url: str, payload: dict) -> dict:
            self.assertEqual(url, sbom.OSV_URL)
            self.queries.append(payload)
            return {"results": [{"vulns": [{"id": vuln} for vuln in vulnerable.get(query["commit"], [])]}
                                if vulnerable.get(query["commit"]) else {} for query in payload["queries"]]}
        return post

    def test_every_pinned_commit_is_queried_and_a_clean_set_passes(self):
        lines, found = sbom.osv_scan(self.merged(), post=self.fake_osv({}))
        self.assertEqual(found, [])
        self.assertEqual(self.queries, [{"queries": [{"commit": SHA_A}, {"commit": SHA_B}, {"commit": SHA_T}]}])
        self.assertEqual(len(lines), 3)

    def test_a_vulnerable_pin_fails(self):
        _, found = sbom.osv_scan(self.merged(), post=self.fake_osv({SHA_B: ["CVE-2099-1"]}))
        self.assertEqual(found, [f"beta@{SHA_B}: CVE-2099-1"])

    def test_an_ignored_vulnerability_is_reported_but_passes(self):
        lines, found = sbom.osv_scan(self.merged(), post=self.fake_osv({SHA_B: ["CVE-2099-1"]}),
                                     ignores={"CVE-2099-1": "not compiled in"})
        self.assertEqual(found, [])
        self.assertTrue(any("CVE-2099-1 ignored: not compiled in" in line for line in lines), lines)

    def test_a_document_without_the_cpp_components_fails(self):
        with self.assertRaisesRegex(sbom.DependencyError, "no pinned C/C\\+\\+ dependency"):
            sbom.osv_scan(json.loads(json.dumps(SYFT)), post=self.fake_osv({}))

    def test_a_short_answer_fails(self):
        with self.assertRaisesRegex(sbom.DependencyError, "answered 0 of 3"):
            sbom.osv_scan(self.merged(), post=lambda _url, _payload: {"results": []})

    def test_ignores_need_a_reason_and_lapse_on_their_date(self):
        self.write("osv-scanner.toml", '[[IgnoredVulns]]\nid = "CVE-2099-1"\nignoreUntil = 2099-01-31\nreason = "why"\n')
        self.assertIn("CVE-2099-1", sbom.osv_ignores(self.root, today=datetime.date(2099, 1, 31)))
        self.assertEqual(sbom.osv_ignores(self.root, today=datetime.date(2099, 2, 1)), {})
        self.write("osv-scanner.toml", '[[IgnoredVulns]]\nid = "CVE-2099-1"\nignoreUntil = 2099-01-31\n')
        with self.assertRaisesRegex(sbom.DependencyError, "needs an id, a reason and an ignoreUntil"):
            sbom.osv_ignores(self.root)
        self.write("osv-scanner.toml", '[[IgnoredVulns]]\nid = "CVE-2099-1"\nreason = "for ever"\n')
        with self.assertRaises(sbom.DependencyError):
            sbom.osv_ignores(self.root)


class RepositoryTest(unittest.TestCase):
    """The checkout's own pins, and the workflows that use them."""

    def test_the_checkout_lists_the_shipped_libraries(self):
        by_name = {dependency.name: dependency for dependency in sbom.components(REPO)}
        for name in ("SDL3", "imgui", "implot", "glad", "freetype", "spdlog", "tomlplusplus", "stb"):
            with self.subTest(name):
                self.assertEqual(by_name[name].scope, "shipped")
                self.assertRegex(by_name[name].purl, r"^pkg:github/[a-z0-9_.-]+/[a-z0-9_.-]+@[0-9a-f]{40}$")
                self.assertNotEqual(by_name[name].license, "NOASSERTION")
        for name in ("googletest", "rapidcheck", "benchmark"):
            self.assertIn(by_name[name].scope, ("test", "benchmark"))
        for name in ("libbpf", "libcurl", "libsystemd"):
            self.assertEqual(by_name[name].scope, "system")

    def test_the_checkout_has_as_many_pins_as_fetchcontent_calls(self):
        calls = sum(len(re.findall(r"^[ \t]*FetchContent_Declare[ \t]*\(", (REPO / relative).read_text(encoding="utf-8"),
                                   re.MULTILINE)) for relative in sbom.SCOPES)
        self.assertEqual(len(sbom.fetchcontent_dependencies(REPO)), calls)

    def test_the_checkouts_ignores_are_valid(self):
        sbom.osv_ignores(REPO, today=datetime.date(2000, 1, 1))

    def test_the_release_merges_and_checks_the_source_sbom(self):
        text = (REPO / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        merge_at = text.index("tools/cpp-deps-sbom.py merge")
        self.assertLess(text.index("name: Generate source SBOM"), merge_at)
        self.assertLess(merge_at, text.index("name: Stage SBOM into dist"))
        check_at = text.index("tools/cpp-deps-sbom.py check")
        self.assertLess(text.index("name: Stage SBOM into dist"), check_at)
        self.assertLess(check_at, text.index("name: Write SHA256SUMS"))

    def test_the_osv_workflow_merges_checks_and_scans_the_cpp_components(self):
        text = (REPO / ".github" / "workflows" / "osv-scanner.yml").read_text(encoding="utf-8")
        for command in ("merge", "check", "osv"):
            self.assertTrue(f"tools/cpp-deps-sbom.py {command} --sbom sbom.spdx.json" in text, command)
        self.assertLess(text.index("tools/cpp-deps-sbom.py merge"), text.index("name: Upload SBOM artifact"))
        for path in ("'tools/cpp-deps-sbom.py'", "'osv-scanner.toml'"):
            self.assertTrue(path in text, path)


if __name__ == "__main__":
    unittest.main()
