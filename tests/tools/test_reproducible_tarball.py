#!/usr/bin/env python3
"""Tests for cmake/ReproducibleTarball.cmake, the Linux .tar.gz's CPack post-build script (#2636).

CPack writes a .tar.gz's entries in readdir order, with the build user's owner and umask, so two
builds of the same commit could differ. Two archives with the same files, written in different
orders with different owners, modes and times, must come out of the script byte-identical, sorted,
owned by root and stamped with SOURCE_DATE_EPOCH. Needs GNU tar and gzip; registered in CTest only
on Linux.
"""

import gzip
import io
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "cmake" / "ReproducibleTarball.cmake"
CMAKE = os.environ.get("TASKSMACK_TEST_CMAKE") or "cmake"
EPOCH = 1792442853

TOP = "TaskSmack-0.9.0-Linux"
# (name, mode, content); content None is a directory
ENTRIES = [
    (f"{TOP}/share", 0o775, None),
    (f"{TOP}/share/tasksmack", 0o775, None),
    (f"{TOP}/share/tasksmack/themes", 0o775, None),
    (f"{TOP}/share/tasksmack/themes/nord.toml", 0o664, b"nord\n"),
    (f"{TOP}/share/tasksmack/themes/arctic-fire.toml", 0o664, b"arctic\n"),
    (f"{TOP}/share/tasksmack/themes/README.md", 0o664, b"readme\n"),
    (f"{TOP}/bin", 0o775, None),
    (f"{TOP}/bin/TaskSmack", 0o775, b"\x7fELF binary\n"),
]


def write_archive(path, entries, uid, uname, mtime):
    """Writes entries in the given order, as CPack would from a staging tree."""
    with tarfile.open(path, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        for name, mode, content in entries:
            info = tarfile.TarInfo(name)
            info.mode = mode
            info.uid = info.gid = uid
            info.uname = info.gname = uname
            info.mtime = mtime
            if content is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))


def repack(path, epoch=EPOCH):
    env = dict(os.environ)
    if epoch is None:
        env.pop("SOURCE_DATE_EPOCH", None)
    else:
        env["SOURCE_DATE_EPOCH"] = str(epoch)
    return subprocess.run(
        [CMAKE, f"-DCPACK_PACKAGE_FILES={path}", "-P", str(SCRIPT)],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )


class ReproducibleTarballTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_different_orders_owners_modes_and_times_give_identical_bytes(self):
        first = self.tmp / "a" / "TaskSmack.tar.gz"
        second = self.tmp / "b" / "TaskSmack.tar.gz"
        first.parent.mkdir()
        second.parent.mkdir()
        write_archive(first, ENTRIES, 1001, "runner", EPOCH)
        reordered = list(reversed(ENTRIES[3:6])) + ENTRIES[6:] + ENTRIES[:3]
        write_archive(second, reordered, 1000, "someone", EPOCH + 3600)
        self.assertNotEqual(first.read_bytes(), second.read_bytes())

        for path in (first, second):
            result = repack(path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertFalse((first.parent / "_tasksmack_repack").exists())

        data = first.read_bytes()
        # gzip header: no file name (FLG 0) and no timestamp (MTIME 0)
        self.assertEqual(data[3], 0)
        self.assertEqual(data[4:8], b"\0\0\0\0")
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data))) as tar:
            members = tar.getmembers()
        names = [m.name.rstrip("/") for m in members]
        self.assertEqual(names, [TOP] + sorted(name for name, _, _ in ENTRIES))
        for member in members:
            self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, "", ""), member.name)
            self.assertEqual(member.mtime, EPOCH, member.name)
            expected = 0o755 if member.isdir() or member.name.endswith("/bin/TaskSmack") else 0o644
            self.assertEqual(member.mode, expected, member.name)
        with tarfile.open(first) as tar:
            self.assertEqual(tar.extractfile(f"{TOP}/bin/TaskSmack").read(), b"\x7fELF binary\n")

    def test_without_source_date_epoch_keeps_the_entry_times(self):
        path = self.tmp / "TaskSmack.tar.gz"
        write_archive(path, ENTRIES, 1001, "runner", EPOCH - 86400)
        result = repack(path, epoch=None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with tarfile.open(path) as tar:
            files = [m for m in tar.getmembers() if m.isfile()]
        self.assertTrue(files)
        for member in files:
            self.assertEqual(member.mtime, EPOCH - 86400, member.name)

    def test_other_package_files_are_left_alone(self):
        deb = self.tmp / "TaskSmack.deb"
        deb.write_bytes(b"not an archive")
        result = repack(deb)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(deb.read_bytes(), b"not an archive")


if __name__ == "__main__":
    unittest.main()
