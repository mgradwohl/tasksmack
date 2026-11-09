#!/usr/bin/env python3
"""Tests for the "TaskSmack (Administrator)" launcher and its polkit action (#3580).

pkexec starts a program only when a polkit action's exec.path names it exactly, and keeps the display
only when that action sets allow_gui. So the launcher's Exec must be pkexec and the very path the action
names, which must be where the .deb and .rpm install TaskSmack. Both files are installed only in the
"administrator" component, which only those packages carry.
"""

import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ASSETS = REPO_ROOT / "assets" / "linux"
LAUNCHER = ASSETS / "app.tasksmack.TaskSmack.Administrator.desktop"
POLICY = ASSETS / "app.tasksmack.TaskSmack.Administrator.policy"
STANDARD = ASSETS / "app.tasksmack.TaskSmack.desktop"
INSTALL_RULES = REPO_ROOT / "cmake" / "InstallRules.cmake"
CPACK_CONFIG = REPO_ROOT / "cmake" / "CPackProjectConfig.cmake.in"
INSTALLED_TASKSMACK = "/usr/bin/TaskSmack"


def desktop_entry(path):
    """The [Desktop Entry] group's keys."""
    entry = {}
    group = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("["):
            group = line.strip()
        elif group == "[Desktop Entry]" and "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            entry[key] = value
    return entry


def annotations(action):
    return {a.get("key"): (a.text or "").strip() for a in action.findall("annotate")}


class AdministratorLauncherTest(unittest.TestCase):
    def setUp(self):
        self.entry = desktop_entry(LAUNCHER)
        root = ET.parse(POLICY).getroot()
        actions = root.findall("action")
        self.assertEqual(len(actions), 1)
        self.action = actions[0]

    def test_the_launcher_runs_pkexec_on_the_installed_tasksmack(self):
        self.assertEqual(self.entry["Exec"], f"pkexec {INSTALLED_TASKSMACK}")
        self.assertEqual(self.entry["Type"], "Application")
        self.assertEqual(self.entry["Terminal"], "false")
        self.assertEqual(self.entry["Name"], "TaskSmack (Administrator)")

    def test_the_action_names_that_path_and_keeps_the_display(self):
        notes = annotations(self.action)
        self.assertEqual(notes["org.freedesktop.policykit.exec.path"], INSTALLED_TASKSMACK)
        self.assertEqual(notes["org.freedesktop.policykit.exec.allow_gui"], "true")
        self.assertEqual(self.action.get("id"), "app.tasksmack.TaskSmack.administrator")

    def test_every_launch_asks_for_an_administrator_password(self):
        defaults = self.action.find("defaults")
        for key in ("allow_any", "allow_inactive", "allow_active"):
            self.assertEqual(defaults.find(key).text, "auth_admin", key)

    def test_it_shares_the_standard_launchers_icon_and_window_class(self):
        standard = desktop_entry(STANDARD)
        for key in ("Icon", "StartupWMClass", "Categories"):
            self.assertEqual(self.entry[key], standard[key], key)

    def test_only_the_system_packages_install_both_files(self):
        rules = INSTALL_RULES.read_text(encoding="utf-8")
        for name in (LAUNCHER.name, POLICY.name):
            match = re.search(re.escape(name) + r"\s+DESTINATION\s+\S+\s+COMPONENT\s+(\w+)", rules)
            self.assertIsNotNone(match, name)
            self.assertEqual(match.group(1), "administrator", name)
        packages = re.search(r'if\(CPACK_GENERATOR STREQUAL "DEB" OR CPACK_GENERATOR STREQUAL "RPM"\)\s*\n\s*set\(CPACK_INSTALL_CMAKE_PROJECTS "([^"]*)"',
                             CPACK_CONFIG.read_text(encoding="utf-8"))
        self.assertIsNotNone(packages)
        self.assertIn(";administrator;/", packages.group(1))


if __name__ == "__main__":
    unittest.main()
