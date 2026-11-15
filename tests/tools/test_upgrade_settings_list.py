#!/usr/bin/env python3
"""Tests for the one list of settings an upgrade must keep (#3831).

tests/App/fixtures/config-v0.8.0.expected.txt lists each setting v0.8.0's config.toml changed from the
defaults, its value as TaskSmack writes it, and whether it is "stable" (written the same way by every release
since) or "migrated" (only a build that has migrated the file writes it). release.yml's validate-linux-upgrade,
tools/release-smoke-check.sh, tools/release-smoke-check.ps1 and tools/windows-upgrade-check.ps1 all read it,
so a setting added to the fixture is added once. This test keeps the list well formed and equal to the fixture,
and keeps a copy of it from growing back in the readers.
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "App" / "fixtures"
EXPECTED = FIXTURES / "config-v0.8.0.expected.txt"
FIXTURE = FIXTURES / "config-v0.8.0.toml"
READERS = [
    ROOT / "tools" / "release-smoke-check.sh",
    ROOT / "tools" / "release-smoke-check.ps1",
    ROOT / "tools" / "windows-upgrade-check.ps1",
]
RELEASE = ROOT / ".github" / "workflows" / "release.yml"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def expected_settings() -> list[tuple[str, str, str, str]]:
    settings = []
    for line in read(EXPECTED).splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        settings.append(tuple(line.split(" ")))
    return settings


def toml_value(section: str, key: str) -> str | None:
    current = ""
    for line in read(FIXTURE).splitlines():
        header = re.fullmatch(r"\[(.+)\]", line)
        if header:
            current = header.group(1)
            continue
        match = re.fullmatch(r"(\S+) = (.*)", line)
        if match and current == section and match.group(1) == key:
            return match.group(2)
    return None


class UpgradeSettingsListTest(unittest.TestCase):
    def test_every_line_has_four_columns(self) -> None:
        settings = expected_settings()
        self.assertGreater(len(settings), 0)
        for entry in settings:
            self.assertEqual(len(entry), 4, entry)
            self.assertIn(entry[3], ("stable", "migrated"), entry)

    def test_stable_settings_are_what_the_fixture_holds(self) -> None:
        stable = [entry for entry in expected_settings() if entry[3] == "stable"]
        self.assertGreater(len(stable), 0)
        for section, key, value, _ in stable:
            self.assertEqual(toml_value(section, key), value, f"[{section}] {key}")

    def test_migrated_settings_are_not_in_the_fixture(self) -> None:
        for section, key, _, when in expected_settings():
            if when == "migrated":
                self.assertIsNone(toml_value(section, key), f"[{section}] {key} is in the fixture, so it is stable")

    def test_no_reader_keeps_a_list_of_its_own(self) -> None:
        for path in [*READERS, RELEASE]:
            text = read(path)
            for section, key, _, _ in expected_settings():
                self.assertNotRegex(text, rf"['\"\(,]\s*{section}['\"]?,?\s+'?{key}", f"{path.name} lists [{section}] {key} itself")

    def test_every_reader_names_the_list(self) -> None:
        for path in READERS:
            self.assertIn(".expected.txt", read(path), path.name)

    def test_every_job_that_checks_out_the_fixture_for_a_reader_also_checks_out_the_list(self) -> None:
        # A sparse checkout that names the fixture and a reader (or the upgrade step) must name the list too.
        text = read(RELEASE)
        blocks = re.findall(r"sparse-checkout: \|?\n?((?:\s+\S.*\n)+?)\s+sparse-checkout-cone-mode", text)
        needing = 0
        for block in blocks:
            if "config-v0.8.0.toml" in block:
                needing += 1
                self.assertIn("config-v0.8.0.expected.txt", block, block)
        self.assertGreaterEqual(needing, 5)


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=2))
