#!/usr/bin/env python3
"""
tools/bench-ps1-launch.py -- One further launch of bench.ps1 for bench-across-launches.py run (#4198).

Usage:
    bench-ps1-launch.py ARGS.json COMMAND... --benchmark_filter=REGEX

bench-across-launches.py run appends --benchmark_filter=<the plan's filter> to the command it launches.
For bench.ps1 that argument can't go on a "pwsh -File" command line: pwsh splits an argument that starts
with "-" at its first ":" as -name:value, and most flagged names hold one (BM_X/iterations:1000, #3547).
So the command is this script, and the filter, its last argument, is added to the benchmark arguments in
ARGS.json (a UTF-8 JSON array of strings: bench.ps1's own, as the caller gave them) and the result is
handed to COMMAND (pwsh -File bench.ps1 ...) as -ExtraArgsFile, a temporary file removed afterwards. Google
Benchmark takes the last --benchmark_filter, so the plan's wins over one in ARGS.json.

The output and the exit code are COMMAND's. TASKSMACK_BENCH_OUT_DIR, which run sets for the launch, reaches
bench.ps1 through the environment.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) < 4 or not argv[-1].startswith("--benchmark_filter="):
        print("usage: bench-ps1-launch.py ARGS.json COMMAND... --benchmark_filter=REGEX", file=sys.stderr)
        return 2
    base = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    if not isinstance(base, list) or not all(isinstance(item, str) for item in base):
        print(f"ERROR: {argv[0]} is not a JSON array of strings", file=sys.stderr)
        return 2
    handle, name = tempfile.mkstemp(prefix="tasksmack-bench-args-", suffix=".json")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump([*base, argv[-1]], stream, ensure_ascii=False)
        try:
            return subprocess.run([*argv[1:-1], "-ExtraArgsFile", name], check=False).returncode
        except OSError as error:
            print(f"ERROR: cannot start {argv[1]}: {error}", file=sys.stderr)
            return 127
    finally:
        os.unlink(name)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
