#!/usr/bin/env python3
"""Write the provenance manifest that tools/bench.sh puts next to each benchmark result (#1424).

The field names match tools/bench.ps1's manifest, so one consumer can read both. Only an
anonymized machine class is recorded (CPU model, logical core count, OS): never the host name,
user name, other processes or command lines beyond the benchmark's own arguments, and paths are
reduced to file names.

Usage:
    bench-manifest.py --manifest OUT --result RESULT_JSON --binary BIN --preset P \
        --exit-code N --repo-root DIR -- <benchmark args...>
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path, PurePath


def git_provenance(repo_root: Path) -> dict:
    def git(*args: str) -> str | None:
        try:
            result = subprocess.run(
                ["git", "-C", str(repo_root), *args], capture_output=True, text=True, check=False
            )
        except OSError:
            return None
        return result.stdout.strip() if result.returncode == 0 else None

    commit = git("rev-parse", "HEAD")
    if commit is None:
        return {"commit": None, "branch": None, "dirty": None}
    # Tracked changes only: untracked scratch files do not change what was built.
    status = git("status", "--porcelain", "--untracked-files=no")
    return {
        "commit": commit,
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": None if status is None else bool(status),
    }


def build_provenance(binary: Path) -> dict:
    """Read build config from the CMakeCache.txt of the binary's build tree (build/<preset>)."""
    build: dict = {
        "build_type": None,
        "generator": None,
        "compiler": None,
        "compiler_id": None,
        "compiler_version": None,
        "cxx_flags": None,
        "cxx_flags_config": None,
        "ipo": None,
    }
    build_dir = binary.resolve().parent.parent
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return build
    cache: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_]+)(?::[A-Za-z]+)?=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    build["build_type"] = cache.get("CMAKE_BUILD_TYPE")
    build["generator"] = cache.get("CMAKE_GENERATOR")
    # Only the compiler's file name: its full path can sit under a user's home directory.
    if cache.get("CMAKE_CXX_COMPILER"):
        build["compiler"] = PurePath(cache["CMAKE_CXX_COMPILER"].replace("\\", "/")).name
    build["cxx_flags"] = cache.get("CMAKE_CXX_FLAGS")
    if build["build_type"]:
        build["cxx_flags_config"] = cache.get(f"CMAKE_CXX_FLAGS_{build['build_type'].upper()}")
    build["ipo"] = cache.get("CMAKE_INTERPROCEDURAL_OPTIMIZATION", cache.get("TASKSMACK_ENABLE_IPO"))
    for compiler_file in sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        text = compiler_file.read_text(encoding="utf-8", errors="replace")
        if match := re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text):
            build["compiler_id"] = match.group(1)
        if match := re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text):
            build["compiler_version"] = match.group(1)
        break
    return build


def cpu_model() -> str | None:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("model name"):
                return " ".join(line.split(":", 1)[1].split())
    if sys.platform == "darwin":
        try:
            result = subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True, check=False
            )
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()
        except OSError:
            pass
    return platform.processor() or None


def machine_class() -> dict:
    cpu = cpu_model()
    cores = os.cpu_count()
    os_name = platform.system() or "unknown"
    os_version = platform.release() or None
    return {
        "label": f"{cpu or 'unknown CPU'} / {cores} logical cores / {os_name} {os_version or ''}".rstrip(),
        "cpu_model": cpu,
        "logical_cores": cores,
        "os_name": os_name,
        "os_version": os_version,
        "arch": platform.machine() or None,
    }


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def recorded_args(args: list[str]) -> list[str]:
    """The benchmark's arguments, with the output path reduced to its file name."""
    out = []
    for arg in args:
        if arg.startswith("--benchmark_out="):
            arg = "--benchmark_out=" + PurePath(arg.split("=", 1)[1].replace("\\", "/")).name
        out.append(arg)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--preset", required=True)
    parser.add_argument("--exit-code", type=int, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--generator", default="tools/bench.sh")
    parser.add_argument("bench_args", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    bench_args = options.bench_args
    if bench_args and bench_args[0] == "--":
        bench_args = bench_args[1:]

    manifest = {
        "schema_version": 1,
        "generator": options.generator,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "preset": options.preset,
        "result_file": options.result.name,
        "exit_code": options.exit_code,
        "git": git_provenance(options.repo_root),
        "binary": {"name": options.binary.name, "sha256": sha256_of(options.binary)},
        "build": build_provenance(options.binary),
        "benchmark": {
            "args": recorded_args(bench_args),
            "raw_repetitions": "--benchmark_report_aggregates_only=true" not in bench_args,
            "report_aggregates_only": "--benchmark_report_aggregates_only=true" in bench_args,
        },
        "machine": machine_class(),
    }
    options.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
