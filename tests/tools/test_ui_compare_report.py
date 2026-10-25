#!/usr/bin/env python3
"""Tests for tools/ui-compare-report.py (#2712).

The PNG reader must decode every filter type stb_image_write may pick for a row, the diff must count
only changes past its threshold, and the report must list a step one side lacks rather than fail.
"""

import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "ui-compare-report.py"
spec = importlib.util.spec_from_file_location("ui_compare_report", SCRIPT)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def solid(width: int, height: int, rgba: tuple[int, int, int, int]) -> "report.Image":
    return report.Image(width, height, bytes(rgba) * (width * height))


def png_with_filters(width: int, rows: list[bytes], filters: list[int]) -> bytes:
    """An RGBA PNG whose row y is stored with filter type filters[y], encoded here independently of the
    module under test."""

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    def paeth(a: int, b: int, c: int) -> int:
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    stride = width * 4
    encoded = bytearray()
    previous = bytes(stride)
    for row, kind in zip(rows, filters):
        out = bytearray()
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            up = previous[x]
            up_left = previous[x - 4] if x >= 4 else 0
            predictor = (0, left, up, (left + up) >> 1, paeth(left, up, up_left))[kind]
            out.append((row[x] - predictor) & 0xFF)
        encoded += bytes((kind,)) + out
        previous = row
    header = struct.pack(">IIBBBBB", width, len(rows), 8, 6, 0, 0, 0)
    return report.PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(encoded))) + chunk(b"IEND", b"")


class PngTest(unittest.TestCase):
    def test_an_image_round_trips(self) -> None:
        image = report.Image(2, 2, bytes(range(16)))
        decoded = report.read_png(report.write_png(image))
        self.assertEqual((decoded.width, decoded.height, decoded.rgba), (2, 2, image.rgba))

    def test_every_filter_type_decodes(self) -> None:
        rows = [bytes((x * 37 + y * 11) & 0xFF for x in range(12)) for y in range(5)]
        decoded = report.read_png(png_with_filters(3, rows, [0, 1, 2, 3, 4]))
        self.assertEqual(decoded.rgba, b"".join(rows))

    def test_anything_but_8_bit_rgba_is_refused(self) -> None:
        data = bytearray(report.write_png(solid(1, 1, (0, 0, 0, 255))))
        data[8 + 8 + 9] = 2  # IHDR's colour type: RGB
        with self.assertRaises(ValueError):
            report.read_png(bytes(data))
        with self.assertRaises(ValueError):
            report.read_png(b"not a png")


class DiffTest(unittest.TestCase):
    def test_identical_frames_change_nothing(self) -> None:
        _, percent = report.diff_images(solid(4, 4, (10, 20, 30, 255)), solid(4, 4, (10, 20, 30, 255)))
        self.assertEqual(percent, 0.0)

    def test_a_change_within_the_threshold_is_noise(self) -> None:
        shifted = report.CHANGE_THRESHOLD
        _, percent = report.diff_images(solid(4, 4, (100, 100, 100, 255)), solid(4, 4, (100 + shifted, 100, 100, 255)))
        self.assertEqual(percent, 0.0)

    def test_changed_pixels_are_counted_and_drawn_red(self) -> None:
        a = solid(2, 2, (0, 0, 0, 255))
        b_pixels = bytearray(a.rgba)
        b_pixels[0:3] = b"\xff\xff\xff"  # the first pixel only
        diff, percent = report.diff_images(a, report.Image(2, 2, bytes(b_pixels)))
        self.assertEqual(percent, 25.0)
        self.assertEqual(diff.rgba[0:4], b"\xff\x30\x30\xff")
        self.assertNotEqual(diff.rgba[4:8], b"\xff\x30\x30\xff")

    def test_frames_of_different_sizes_count_the_uncovered_area_as_changed(self) -> None:
        diff, percent = report.diff_images(solid(2, 2, (0, 0, 0, 255)), solid(4, 2, (0, 0, 0, 255)))
        self.assertEqual((diff.width, diff.height), (4, 2))
        self.assertEqual(percent, 50.0)


class ReportTest(unittest.TestCase):
    def test_the_report_pairs_the_frames_lists_a_missing_step_and_quotes_the_warnings(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "a").mkdir()
            (out / "b").mkdir()
            frame = report.write_png(solid(3, 3, (50, 60, 70, 255)))
            (out / "a" / "01-list.png").write_bytes(frame)
            (out / "b" / "01-list.png").write_bytes(frame)
            (out / "a" / "02-tree.png").write_bytes(frame)
            (out / "b" / "run.log").write_text("[info] fine\n[warning] Theme not found: x\nIM_ASSERT failed\n", encoding="utf-8")
            self.assertEqual(report.build_report(out, "main", "bump"), 2)
            page = (out / "index.html").read_text(encoding="utf-8")
            self.assertIn("0.00% changed", page)
            self.assertIn("only one side", page)
            self.assertIn("Theme not found: x", page)
            self.assertIn("IM_ASSERT failed", page)
            self.assertNotIn("fine", page.split("bump: warnings and errors")[1])
            self.assertTrue((out / "diff" / "01-list.png").is_file())
            self.assertFalse((out / "diff" / "02-tree.png").exists())

    def test_a_line_ctest_printed_twice_is_quoted_once(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "run.log"
            log.write_text("6: [warning] odd\n6: [info] fine\n[warning] odd\n", encoding="utf-8")
            self.assertEqual(report.noteworthy_lines(log), ["[warning] odd"])

    def test_no_frames_on_either_side_is_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(report.main(["ui-compare-report.py", tmp, "a", "b"]), 1)


if __name__ == "__main__":
    sys.exit(unittest.main())
