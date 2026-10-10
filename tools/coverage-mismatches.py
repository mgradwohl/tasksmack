#!/usr/bin/env python3
"""
tools/coverage-mismatches.py -- Explain llvm-cov's "N functions have mismatched data" warning.

Usage:
    tools/coverage-mismatches.py --profdata <default.profdata> --llvm-profdata <tool> \
        --llvm-objcopy <tool> <binary> [<binary> ...]

llvm-cov drops a function's coverage record when the profile has data for that function name but
not under the record's structural hash, and counts it in that warning. tools/coverage.sh merges
three binaries into one report, and every run warns about a couple of hundred functions (#1544).

They are clang's "unused function" placeholders: a header or inline function a binary references
but never emits gets a record with hash 0 and no counters. llvm-cov keeps a placeholder only when
the binary has no real record of the same name, so the ones that reach the profile lookup are
functions some *other* binary emitted and ran -- the profile has them under that binary's real
hash, and that binary's real record carries the counts. A placeholder has nothing to lose, so the
warning is harmless as long as every mismatched record is a placeholder.

This script repeats llvm-cov's lookup for each binary and sorts the mismatches into placeholders
and real records. A real-record mismatch -- a function compiled with a different structural hash
in two binaries -- would drop real counts, and is reported as an error (exit status 1).

ELF only: it reads the __llvm_covfun section. Records are as of LLVM 22 (coverage mapping format
version 6): name MD5 (u64), data size (u32), function hash (u64), filenames hash (u64), data,
8-byte aligned.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

RECORD_HEADER = struct.Struct("<QIQQ")


def name_md5(name: str) -> int:
    """The 64-bit name reference llvm stores for a function: the low 8 bytes of MD5(name)."""
    return struct.unpack("<Q", hashlib.md5(name.encode()).digest()[:8])[0]


def profile_hashes(llvm_profdata: str, profdata: Path) -> dict[int, set[int]]:
    """Function name MD5 -> the structural hashes the profile holds counters under."""
    text = subprocess.run(
        [llvm_profdata, "show", "--all-functions", "--text", str(profdata)],
        check=True,
        capture_output=True,
        text=True,
        errors="replace",
    ).stdout
    result: dict[int, set[int]] = defaultdict(set)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        # Each function is: name, "# Func Hash:", hash, "# Num Counters:", ...
        if line == "# Func Hash:" and index > 0 and index + 1 < len(lines):
            result[name_md5(lines[index - 1])].add(int(lines[index + 1]))
    return result


def coverage_records(llvm_objcopy: str, binary: Path) -> list[tuple[int, int]]:
    """(name MD5, function hash) for every coverage record in the binary."""
    with tempfile.TemporaryDirectory() as scratch:
        section = Path(scratch) / "covfun.bin"
        stripped = Path(scratch) / "out.o"
        subprocess.run(
            [llvm_objcopy, f"--dump-section=__llvm_covfun={section}", str(binary), str(stripped)],
            check=True,
            capture_output=True,
        )
        return parse_records(section.read_bytes())


def parse_records(blob: bytes) -> list[tuple[int, int]]:
    """(name MD5, function hash) for each record of a __llvm_covfun section's contents."""
    records = []
    offset = 0
    while offset + RECORD_HEADER.size <= len(blob):
        name_ref, data_size, func_hash, _filenames_ref = RECORD_HEADER.unpack_from(blob, offset)
        offset = (offset + RECORD_HEADER.size + data_size + 7) & ~7
        if name_ref != 0 or data_size != 0:
            records.append((name_ref, func_hash))
    return records


def classify(records: list[tuple[int, int]], profile: dict[int, set[int]]) -> tuple[int, int]:
    """(placeholder mismatches, real-record mismatches), as llvm-cov would count them."""
    # llvm-cov's reader keeps a hash-0 record only when the binary has no real record of that name,
    # and loads each (name, hash) once.
    real_names = {name for name, func_hash in records if func_hash != 0}
    placeholders = 0
    real = 0
    for name, func_hash in set(records):
        if func_hash == 0 and name in real_names:
            continue
        hashes = profile.get(name)
        if hashes is None or func_hash in hashes:
            continue
        if func_hash == 0:
            placeholders += 1
        else:
            real += 1
    return placeholders, real


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--profdata", required=True, type=Path)
    parser.add_argument("--llvm-profdata", required=True)
    parser.add_argument("--llvm-objcopy", required=True)
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()

    profile = profile_hashes(args.llvm_profdata, args.profdata)
    total_placeholders = 0
    total_real = 0
    for binary in args.binaries:
        placeholders, real = classify(coverage_records(args.llvm_objcopy, binary), profile)
        total_placeholders += placeholders
        total_real += real
        print(f"  {binary.name}: {placeholders} unused-function placeholder(s), {real} real record(s)")

    print(
        f"Mismatched functions: {total_placeholders + total_real} "
        f"({total_placeholders} harmless unused-function placeholders, {total_real} real)"
    )
    if total_real:
        print(
            f"error: {total_real} function(s) are compiled with a different structural hash in different "
            "binaries; their counts are missing from the report (#1544).",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
