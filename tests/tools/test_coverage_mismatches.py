#!/usr/bin/env python3
"""Tests for tools/coverage-mismatches.py: sorting llvm-cov's mismatched functions into harmless
unused-function placeholders and real-record mismatches that drop counts (#1544)."""

import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "coverage-mismatches.py"
SPEC = importlib.util.spec_from_file_location("coverage_mismatches", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

INLINE = MODULE.name_md5("_ZN2UI7Widgets4helpEv")
OTHER = MODULE.name_md5("_ZN6Domain5Model6sampleEv")


class NameMd5Test(unittest.TestCase):
    def test_is_the_low_eight_bytes_of_md5_little_endian(self):
        # MD5("main") = fad58de7366495db4650cfefac2fcd61; llvm reads the first 8 bytes as a little-endian u64.
        self.assertEqual(MODULE.name_md5("main"), 0xDB956436E78DD5FA)


class RecordParsingTest(unittest.TestCase):
    def test_reads_each_record_and_skips_its_aligned_data(self):
        def record(name, func_hash, data):
            raw = MODULE.RECORD_HEADER.pack(name, len(data), func_hash, 0) + data
            return raw + b"\0" * (-len(raw) % 8)

        blob = record(INLINE, 0, b"\1\2\3") + record(OTHER, 42, b"\4" * 8)
        self.assertEqual(MODULE.parse_records(blob), [(INLINE, 0), (OTHER, 42)])

    def test_ignores_zero_padding_at_the_end(self):
        self.assertEqual(MODULE.parse_records(b"\0" * MODULE.RECORD_HEADER.size), [])


class ClassifyTest(unittest.TestCase):
    def test_a_placeholder_for_a_function_another_binary_ran_is_harmless(self):
        # The app references UI::Widgets::help but never emits it; the tests ran it under hash 7.
        self.assertEqual(MODULE.classify([(INLINE, 0)], {INLINE: {7}}), (1, 0))

    def test_a_placeholder_beside_a_real_record_is_not_looked_up(self):
        # llvm-cov's reader drops the placeholder when the binary has a real record of that name.
        self.assertEqual(MODULE.classify([(INLINE, 0), (INLINE, 7)], {INLINE: {7}}), (0, 0))

    def test_a_real_record_under_another_hash_drops_counts(self):
        self.assertEqual(MODULE.classify([(OTHER, 99)], {OTHER: {42}}), (0, 1))

    def test_matching_and_unprofiled_functions_are_not_mismatches(self):
        records = [(OTHER, 42), (INLINE, 0)]
        self.assertEqual(MODULE.classify(records, {OTHER: {42}}), (0, 0))

    def test_a_function_profiled_under_several_hashes_matches_any_of_them(self):
        self.assertEqual(MODULE.classify([(OTHER, 99)], {OTHER: {42, 99}}), (0, 0))

    def test_duplicate_records_count_once(self):
        self.assertEqual(MODULE.classify([(INLINE, 0), (INLINE, 0)], {INLINE: {7}}), (1, 0))


if __name__ == "__main__":
    unittest.main()
