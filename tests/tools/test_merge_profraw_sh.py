#!/usr/bin/env python3
"""Tests for tools/merge-profraw.sh, coverage.sh's merge step (#4080).

llvm-profdata merge fails the whole merge when any input is unreadable, by default, so one truncated
profile (a child process ended before writing its header) failed a coverage run whose tests all passed.
The script merges with --failure-mode=all, names each skipped profile, and still fails when nothing can
be merged. A stub llvm-profdata stands in, as the real one would need instrumented binaries: it reads a
file holding "good" as a profile, warns about any other as the real one does, and fails like it when
none is good, or when it isn't given --failure-mode=all and one is bad. Needs bash; Linux only, as
coverage.sh is.
"""

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "merge-profraw.sh"
BASH = os.environ.get("TASKSMACK_TEST_BASH") or shutil.which("bash") or "bash"

STUB = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    [[ "$1" == merge ]] || exit 2
    shift
    mode=any out="" inputs=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -sparse) ;;
            --failure-mode=*) mode="${1#--failure-mode=}" ;;
            -o) out="$2"; shift ;;
            *) inputs+=("$1") ;;
        esac
        shift
    done
    good=0
    for f in "${inputs[@]}"; do
        if [[ "$(cat "$f")" == good ]]; then
            good=$((good + 1))
        else
            echo "warning: $f: invalid instrumentation profile data (file header is corrupt)" >&2
            [[ "$mode" == any ]] && { echo "error: no profile can be merged" >&2; exit 1; }
        fi
    done
    if [[ $good -eq 0 ]]; then echo "error: no profile can be merged" >&2; exit 1; fi
    printf 'merged %d\\n' "$good" > "$out"
    """
)


class MergeProfrawShTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="tasksmack-merge-profraw-"))
        self.stub = self.dir / "llvm-profdata"
        self.stub.write_text(STUB)
        self.stub.chmod(0o755)
        self.out = self.dir / "default.profdata"

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def profile(self, name, content):
        path = self.dir / name
        path.write_text(content)
        return path

    def merge(self, *profiles):
        return subprocess.run(
            [BASH, str(SCRIPT), str(self.stub), str(self.out), *map(str, profiles)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_good_profiles_merge(self):
        result = self.merge(self.profile("a.profraw", "good"), self.profile("b.profraw", "good"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.out.read_text().strip(), "merged 2")
        self.assertNotIn("Skipped", result.stderr)

    def test_a_truncated_profile_is_skipped_and_named(self):
        bad = self.profile("coverage-808.profraw", "trunc")
        result = self.merge(self.profile("a.profraw", "good"), bad)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.out.read_text().strip(), "merged 1")
        self.assertIn("Skipped a profile llvm-profdata couldn't read", result.stderr)
        self.assertIn("coverage-808.profraw", result.stderr)
        self.assertIn("Merged 1 of 2 profiles; 1 skipped", result.stderr)

    def test_no_readable_profile_fails(self):
        result = self.merge(self.profile("a.profraw", "trunc"), self.profile("b.profraw", "trunc"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no coverage profile could be merged", result.stderr)
        self.assertIn("2 skipped", result.stderr)

    def test_no_profiles_is_a_usage_error(self):
        result = subprocess.run([BASH, str(SCRIPT), str(self.stub), str(self.out)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
