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
        # Listed only where pkexec is installed: the .deb only recommends it.
        self.assertEqual(self.entry["TryExec"], "pkexec")
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

    def test_the_window_class_is_the_app_id_taskSmack_sets(self):
        # #4451: SDL sends Core::DESKTOP_APP_ID as the Wayland app_id and the X11 WM_CLASS. A desktop file
        # named for it, with it as StartupWMClass, is how a compositor or dock pairs the window with its launcher.
        header = (REPO_ROOT / "src" / "Core" / "AppIdentity.h").read_text(encoding="utf-8")
        match = re.search(r'DESKTOP_APP_ID = "([^"]+)"', header)
        self.assertIsNotNone(match)
        app_id = match.group(1)
        self.assertEqual(STANDARD.name, app_id + ".desktop")
        for path in (STANDARD, LAUNCHER):
            self.assertEqual(desktop_entry(path)["StartupWMClass"], app_id, path.name)
        # Every script that finds TaskSmack's Wayland window by its app_id uses this one (the #4451 follow-up: the
        # Wayland matrix still looked for "TaskSmack" and never found the window).
        matrix = (REPO_ROOT / "tools" / "wayland-matrix.sh").read_text(encoding="utf-8")
        self.assertIn(app_id, matrix)
        self.assertNotRegex(matrix, r'app_id"?\)? ?(==|=) ?"?TaskSmack\b', "an app_id match on the old name")
        source = (REPO_ROOT / "src" / "Core" / "Application.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r"SDL_SetAppMetadata\([^;]*DESKTOP_APP_ID\)", "the app id reaches SDL")

    def test_only_the_system_packages_install_both_files(self):
        rules = INSTALL_RULES.read_text(encoding="utf-8")
        for name in (LAUNCHER.name, POLICY.name):
            match = re.search(re.escape(name) + r"\s+DESTINATION\s+\S+\s+COMPONENT\s+(\w+)(\s+EXCLUDE_FROM_ALL)?", rules)
            self.assertIsNotNone(match, name)
            self.assertEqual(match.group(1), "administrator", name)
            # A plain `cmake --install --prefix P` leaves it out (#3898): both files name /usr/bin/TaskSmack
            # whatever P is. The packages still get it, by naming the component.
            self.assertIsNotNone(match.group(2), f"{name} must be EXCLUDE_FROM_ALL")
        packages = re.search(r'if\(CPACK_GENERATOR STREQUAL "DEB" OR CPACK_GENERATOR STREQUAL "RPM"\)\s*\n\s*set\(CPACK_INSTALL_CMAKE_PROJECTS "([^"]*)"',
                             CPACK_CONFIG.read_text(encoding="utf-8"))
        self.assertIsNotNone(packages)
        self.assertIn(";administrator;/", packages.group(1))


if __name__ == "__main__":
    unittest.main()
