#!/usr/bin/env python3
"""Tests for tools/check-prereqs.sh's optional ccache row (#2350).

ccache is recommended, not required. Whatever a ccache on PATH prints (a two- or three-component
version, an old one, nothing it can parse, or a failure), and when there is none, the checker must
report it and go on to the remaining checks and the summary rather than exit under
set -euo pipefail. Each case runs the checker with PATH set to a stub ccache plus the few
utilities the script needs, so the host's own tools do not change the result. Needs bash;
registered in CTest only where bash is available.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECK_PREREQS_SH = REPO_ROOT / "tools" / "check-prereqs.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"
UTILITIES = ["awk", "cat", "cut", "dirname", "grep", "head", "sed", "sort", "tr", "uname"]
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SUMMARY = re.compile(
    r"^(All mandatory prerequisites are satisfied\.|Some mandatory prerequisites are missing or out of date\.)$",
    re.M,
)


class CheckerTestBase(unittest.TestCase):
    """Runs the checker with PATH set to a stub directory holding only the basic utilities (and the stubs a test adds)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.bin = Path(self._tmp.name) / "bin"
        self.bin.mkdir()
        for name in UTILITIES:
            found = shutil.which(name)
            if found is None:
                self.skipTest(f"{name} not on PATH")
            (self.bin / name).symlink_to(found)

    def tearDown(self):
        self._tmp.cleanup()

    def run_checker(self, extra_env=None):
        env = {"PATH": str(self.bin), "HOME": self._tmp.name, **(extra_env or {})}
        result = subprocess.run(
            [BASH, str(CHECK_PREREQS_SH)], capture_output=True, text=True, env=env, check=False, timeout=60
        )
        out = ANSI.sub("", result.stdout + result.stderr)
        self.assertRegex(out, SUMMARY, f"no summary (exit {result.returncode}):\n{out}")
        # The checks after ccache ran too
        self.assertRegex(out, r"(?m)^git: ", out)
        self.assertLess(out.index("ccache"), out.index("\ngit: "), out)
        return out


class CheckPrereqsCcacheTests(CheckerTestBase):
    def stub_ccache(self, body):
        stub = self.bin / "ccache"
        stub.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
        stub.chmod(0o755)
        return stub

    def ccache_row(self, out):
        rows = [line for line in out.splitlines() if line.startswith("ccache: ")]
        self.assertEqual(len(rows), 1, out)
        return rows[0]

    def test_two_component_version(self):
        stub = self.stub_ccache('echo "ccache version 4.14"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.14 ({stub})")

    def test_three_component_version(self):
        stub = self.stub_ccache('echo "ccache version 4.14.1"; echo "Features: file-storage http-storage"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.14.1 ({stub})")

    def test_minimum_version(self):
        stub = self.stub_ccache('echo "ccache version 4.9.1"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.9.1 ({stub})")

    def test_older_version_warns(self):
        stub = self.stub_ccache('echo "ccache version 4.2"')
        self.assertEqual(self.ccache_row(self.run_checker()), f"ccache: 4.2 ({stub}) - 4.9.1+ recommended")

    def test_unparseable_version(self):
        stub = self.stub_ccache('echo "ccache, but not a version line"')
        self.assertEqual(
            self.ccache_row(self.run_checker()), f"ccache: version not recognized ({stub}) - 4.9.1+ recommended"
        )

    def test_failing_ccache(self):
        stub = self.stub_ccache('echo "broken" >&2; exit 3')
        self.assertEqual(
            self.ccache_row(self.run_checker()), f"ccache: version not recognized ({stub}) - 4.9.1+ recommended"
        )

    def test_absent_ccache(self):
        out = self.run_checker()
        self.assertEqual(self.ccache_row(out), "ccache: not found (recommended for faster rebuilds)")



class CheckPrereqsLibraryTests(CheckerTestBase):
    """#3915: the libraries the Linux build links, through pkg-config: libbpf required, libsystemd and libcurl optional."""

    def stub_pkg_config(self, present):
        stub = self.bin / "pkg-config"
        stub.write_text(
            "#!/bin/sh\n"
            f'case "$2" in {"|".join(present) or "__none__"}) ;; *) exit 1 ;; esac\n'
            'if [ "$1" = --modversion ]; then echo 9.9.9; fi\n',
            encoding="utf-8",
        )
        stub.chmod(0o755)

    def row(self, out, name):
        rows = [line for line in out.splitlines() if line.startswith(f"{name}: ")]
        self.assertEqual(len(rows), 1, out)
        return rows[0]

    def test_every_library_found(self):
        self.stub_pkg_config(["libbpf", "libsystemd", "libcurl"])
        out = self.run_checker()
        self.assertEqual(self.row(out, "libbpf"), "libbpf: 9.9.9")
        self.assertEqual(self.row(out, "libsystemd"), "libsystemd: 9.9.9")
        self.assertEqual(self.row(out, "libcurl"), "libcurl: 9.9.9")

    def test_missing_libbpf_fails_the_check(self):
        self.stub_pkg_config(["libsystemd", "libcurl"])
        out = self.run_checker()
        self.assertEqual(
            self.row(out, "libbpf"), "libbpf: not found (sudo apt install libbpf-dev) - configure fails without it"
        )
        self.assertIn("Some mandatory prerequisites are missing or out of date.", out)

    def test_missing_optional_libraries_only_warn(self):
        self.stub_pkg_config(["libbpf"])
        out = self.run_checker()
        self.assertTrue(self.row(out, "libsystemd").endswith("- optional; the features it serves are turned off"), out)
        self.assertTrue(self.row(out, "libcurl").startswith("libcurl: not found (sudo apt install libcurl4-openssl-dev)"), out)

    def test_no_pkg_config(self):
        out = self.run_checker()
        self.assertEqual(
            self.row(out, "pkg-config"),
            "pkg-config: not found (sudo apt install pkg-config) - configure needs it to find libbpf",
        )


class CheckPrereqsHotspotTests(CheckerTestBase):
    """#4706: hotspot isn't packaged for Ubuntu 26.04, so the hint names Flathub and KDAB's AppImage, never apt."""

    def test_the_not_found_hint_names_what_exists(self):
        match = re.search(r"(?m)^hotspot: .*$", self.run_checker())
        self.assertIsNotNone(match)
        self.assertIn("flatpak install flathub com.kdab.hotspot", match.group(0))
        self.assertNotIn("apt install hotspot", match.group(0))


class CheckPrereqsLldTests(CheckerTestBase):
    """#4705: ld.lld is found as the other LLVM tools are (the LLVM folders before PATH) and held to the clang floor."""

    def stub_lld(self, folder, version):
        folder.mkdir(parents=True, exist_ok=True)
        stub = folder / "ld.lld"
        stub.write_text(f"#!/bin/sh\necho 'Ubuntu LLD {version} (compatible with GNU linkers)'\n", encoding="utf-8")
        stub.chmod(0o755)
        return stub

    def lld_row(self, out):
        match = re.search(r"(?m)^lld: .*$", out)
        self.assertIsNotNone(match, out)
        return match.group(0)

    def test_an_lld_older_than_the_floor_is_flagged(self):
        root = Path(self._tmp.name) / "lib"
        lld = self.stub_lld(root / "llvm-22" / "bin", "22.1.2")
        out = self.run_checker({"TASKSMACK_LLVM_LIB_ROOT": str(root)})
        row = self.lld_row(out)
        self.assertIn("22.1", row)
        self.assertIn(str(lld), row)
        self.assertIn("minimum required", row, "a stale lld no longer reports green")

    def test_the_llvm_folder_is_reported_before_a_path_shadow(self):
        root = Path(self._tmp.name) / "lib"
        lld = self.stub_lld(root / "llvm-23" / "bin", "23.1.5")
        self.stub_lld(self.bin, "22.1.2")  # a /usr/local/bin-style shadow on PATH
        row = self.lld_row(self.run_checker({"TASKSMACK_LLVM_LIB_ROOT": str(root)}))
        self.assertIn(str(lld), row)
        self.assertIn("23.1", row)
        self.assertNotIn("minimum required", row)



class CheckPrereqsIwyuTests(CheckerTestBase):
    """#4708: an IWYU built against the wrong Clang gets the same what-to-do hint as a missing one, naming CI's commit."""

    CI_SHA = re.search(
        r"IWYU_COMMIT_SHA: '([0-9a-f]{40})'", (CHECK_PREREQS_SH.parent.parent / ".github" / "workflows" / "ci.yml").read_text()
    ).group(1)

    def test_a_missing_iwyu_gets_the_hint_with_cis_commit(self):
        out = self.run_checker()
        self.assertRegex(out, r"(?m)^iwyu: not found")
        self.assertIn(self.CI_SHA, out)

    def test_an_iwyu_built_against_another_clang_gets_the_hint_too(self):
        stub = self.bin / "iwyu"
        stub.write_text("#!/bin/sh\necho 'include-what-you-use 0.23 based on Ubuntu clang version 19.1.7'\n", encoding="utf-8")
        stub.chmod(0o755)
        out = self.run_checker()
        self.assertRegex(out, r"(?m)^iwyu: 0\.23 .*built against Clang 19")
        self.assertIn("Hint", out[out.index("iwyu: 0.23"):])
        self.assertIn(self.CI_SHA, out[out.index("iwyu: 0.23"):])



class CheckPrereqsLlvmRowTests(CheckerTestBase):
    """#4714: every LLVM tool reports through one helper, held to the floor and to the major CI pins, as
    tools/test-check-prereqs.ps1 checks check-prereqs.ps1's: below the floor, on the pin, off it, and not found."""

    # As common.sh's pinned_llvm_major() reads it: the rev line after mirrors-clang-format, "frozen: vN." or "vN.".
    _REV = re.search(r"mirrors-clang-format\n\s*(rev:[^\n]*)", (REPO_ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")).group(1)
    PINNED = (re.search(r"frozen:\s*v?(\d+)\.", _REV) or re.search(r"rev:\s*v?(\d+)\.", _REV)).group(1)

    def stub(self, tool, text):
        path = self.bin / tool
        path.write_text(f"#!/bin/sh\necho '{text}'\n", encoding="utf-8")
        path.chmod(0o755)
        return path

    def row(self, out, tool):
        match = re.search(rf"(?m)^{re.escape(tool)}: .*$", out)
        self.assertIsNotNone(match, out)
        return match.group(0)

    def test_below_the_floor_is_flagged(self):
        path = self.stub("clang-tidy", "LLVM version 22.1.2")
        self.assertEqual(self.row(self.run_checker(), "clang-tidy"), f"clang-tidy: 22 ({path}) - minimum required 23")

    def test_on_the_pin_is_green(self):
        path = self.stub("clang-tidy", f"LLVM version {self.PINNED}.1.5")
        self.assertEqual(self.row(self.run_checker(), "clang-tidy"), f"clang-tidy: {self.PINNED} ({path})")

    def test_above_the_floor_but_off_the_pin_is_skew(self):
        other = str(int(self.PINNED) + 1)
        path = self.stub("clang-tidy", f"LLVM version {other}.0.0")
        out = self.run_checker()
        self.assertEqual(self.row(out, "clang-tidy"), f"clang-tidy: {other} ({path}) - CI pins LLVM {self.PINNED}")
        self.assertIn(f"LLVM {self.PINNED} is the version CI pins", out)

    def test_not_found(self):
        self.assertEqual(self.row(self.run_checker(), "clang-tidy"), "clang-tidy: not found")

    def test_coverage_tools_below_the_floor_are_flagged_now(self):
        # They read a version and printed it green whatever it was (#4714).
        path = self.stub("llvm-cov", "Ubuntu LLVM version 21.1.8")
        self.assertEqual(self.row(self.run_checker(), "llvm-cov"), f"llvm-cov: 21 ({path}) - minimum required 23")


if __name__ == "__main__":
    unittest.main()
