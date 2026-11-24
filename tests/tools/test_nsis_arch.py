#!/usr/bin/env python3
"""Tests for the ARM64 installer's refusal to run on x64 Windows (#4446), in cmake/CPackProjectConfig.cmake.in.

Both NSIS installers use NSIS's x86 stub, which runs on x64 and ARM64 alike, so the windows-arm64 installer, downloaded
by hand to an x64 PC, used to install and "succeed", leaving a TaskSmack.exe that won't start. Only the installer for
CPACK_SYSTEM_NAME windows-arm64 checks for native ARM64 (x64.nsh's IsNativeARM64), as the first lines of the install
section (CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS: CPack's template has one .onInit and no hook in it), and exits with
TASKSMACK_WRONG_ARCH_EXIT_CODE before anything changes. The x64 installer, and every other generator, has no such check.

The file is configured and read the way CPack does, by a cmake -P script (the file's @VAR@ values are given on the command
line), once per system name, and the variables CPack would hand to makensis are printed. makensis isn't needed. When a
project.nsi from a real CPack run of the ARM64 build is named in TASKSMACK_NSIS_ARM64_PROJECT, it is checked too.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "cmake" / "CPackProjectConfig.cmake.in"
VERSION = "9.8.7"
EXIT_CODE = "1633"

SCRIPT = """
configure_file("{config}" "{out}/CPackProjectConfig.cmake" @ONLY)
set(CPACK_GENERATOR "{generator}")
set(CPACK_SYSTEM_NAME "{system}")
include("{out}/CPackProjectConfig.cmake")
file(WRITE "{out}/defines.txt" "${{CPACK_NSIS_DEFINES}}")
file(WRITE "{out}/preinstall.txt" "${{CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}}")
"""


def configured(generator: str, system: str) -> tuple[str, str]:
    """CPACK_NSIS_DEFINES and CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS after CPack reads the file for this package."""
    cmake = shutil.which("cmake")
    if cmake is None:
        raise unittest.SkipTest("cmake isn't on PATH")
    with tempfile.TemporaryDirectory() as folder:
        out = Path(folder)
        script = out / "run.cmake"
        script.write_text(SCRIPT.format(config=CONFIG.as_posix(), out=out.as_posix(), generator=generator, system=system), encoding="utf-8")
        values = {
            "CMAKE_BINARY_DIR": out.as_posix(),
            "CMAKE_SOURCE_DIR": REPO.as_posix(),
            "CMAKE_SYSTEM_NAME": "Windows",
            "PROJECT_NAME": "TaskSmack",
            "PROJECT_VERSION": VERSION,
            "TASKSMACK_PACKAGES_WINDOWS_HELPER": "OFF",
            "TASKSMACK_SIGN_AFTER_PACKAGING": "OFF",
            "TASKSMACK_EXPECTED_SIGNER": "",
            "TASKSMACK_EXPECTED_ISSUER": "",
            "TASKSMACK_EXPECTED_EKU": "",
        }
        command = [cmake] + [f"-D{name}={value}" for name, value in values.items()] + ["-P", str(script)]
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
        return (out / "defines.txt").read_text(encoding="utf-8"), (out / "preinstall.txt").read_text(encoding="utf-8")


def lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if line.strip()]


class ArchGuardTest(unittest.TestCase):
    def test_the_arm64_installer_checks_for_native_arm64_first(self) -> None:
        defines, preinstall = configured("NSIS", "windows-arm64")
        self.assertIn("!include x64.nsh", lines(defines))
        self.assertIn("!include LogicLib.nsh", lines(defines))
        self.assertIn(f"!define TASKSMACK_WRONG_ARCH_EXIT_CODE {EXIT_CODE}", lines(defines))
        program = lines(preinstall)
        start = program.index("${IfNot} ${IsNativeARM64}")
        self.assertEqual([line for line in program[:start] if not line.startswith(";")], [], "the check is the first thing the section does")
        end = program.index("${EndIf}")
        guard = program[start + 1 : end]
        self.assertEqual(len(guard), 3)
        self.assertTrue(guard[0].startswith("MessageBox MB_OK|MB_ICONSTOP "), guard[0])
        self.assertTrue(guard[0].endswith(" /SD IDOK"), "a silent install doesn't wait for a click")
        self.assertIn(f"TaskSmack-{VERSION}-win64.exe", guard[0], "the message names the installer to download")
        self.assertEqual(guard[1:], ["SetErrorLevel ${TASKSMACK_WRONG_ARCH_EXIT_CODE}", "Quit"])
        self.assertIn("Call TaskSmackInUseCheck", program[end:], "the other checks follow, and still run on ARM64")
        self.assertNotIn("TASKSMACK_WRONG_ARCH_EXIT_CODE", "".join(program[end:]))

    def test_the_exit_code_is_not_one_of_the_other_codes(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn(f"!define TASKSMACK_WRONG_ARCH_EXIT_CODE {EXIT_CODE}", text)
        for other in ("0", "1", "2", "32", "87"):
            self.assertNotEqual(EXIT_CODE, other)

    def test_the_x64_installer_has_no_check(self) -> None:
        for system in ("win64", "win32", "Windows"):
            defines, preinstall = configured("NSIS", system)
            for text in (defines, preinstall):
                for needle in ("x64.nsh", "IsNativeARM64", "WRONG_ARCH"):
                    self.assertNotIn(needle, text, f"{system}: {needle}")
            self.assertEqual(lines(preinstall)[:2], ["; TaskSmack in use (#4436): the install folder, before anything is written to it.", "Push $INSTDIR"])

    def test_no_other_generator_gets_it(self) -> None:
        defines, preinstall = configured("ZIP", "windows-arm64")
        self.assertNotIn("IsNativeARM64", defines + preinstall)

    @unittest.skipUnless(os.environ.get("TASKSMACK_NSIS_ARM64_PROJECT"), "no ARM64 CPack project.nsi named")
    def test_generated_project_nsi(self) -> None:
        text = Path(os.environ["TASKSMACK_NSIS_ARM64_PROJECT"]).read_text(encoding="utf-8-sig")
        self.assertIn("!include x64.nsh", text)
        self.assertIn(f"!define TASKSMACK_WRONG_ARCH_EXIT_CODE {EXIT_CODE}", text)
        install = [line.strip() for line in text[text.index('Section "-Core installation"') :].splitlines()]
        guard = install.index("${IfNot} ${IsNativeARM64}")
        self.assertLess(guard, install.index("Call TaskSmackInUseCheck"))
        self.assertLess(guard, min(i for i, line in enumerate(install) if line.startswith(("File ", "ExecWait", "WriteReg"))))


if __name__ == "__main__":
    unittest.main()
