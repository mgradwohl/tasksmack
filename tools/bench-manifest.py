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
from collections.abc import Mapping
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


# An absolute path inside a flag: a drive or UNC path, or a POSIX path of two or more segments
# (so MSVC-style switches such as /DWIN32 are left alone). It may follow the start, whitespace,
# '=' or ',', optionally with a switch glued on (-I/x, -LC:/x, -isystem/x, /IC:\x), which is kept.
# Quoted paths, which can hold spaces, are handled first. Kept in step with Hide-AbsolutePaths in tools/bench.ps1.
_QUOTED_PATH = re.compile(r"""(["'])((?:[A-Za-z]:[\\/]|\\\\|/)[^"']*)\1""")
_BARE_PATH = re.compile(
    r"""(?P<pre>(?:^|[\s=,])(?:-(?:isystem|idirafter|iquote|imsvc|[A-Za-z])|/I)?)(?P<path>(?:[A-Za-z]:[\\/]|\\\\)[^\s"']*|/[^/\s"']+/[^\s"']*)"""
)


def hide_absolute_paths(flags: str | None, repo_root: Path) -> str | None:
    """Replace absolute paths in compiler flags so no user profile or checkout path is recorded.

    A path inside the source tree becomes <source>/relative/path (the PGO presets embed
    ${sourceDir}/profiles/tasksmack.profdata); any other absolute path becomes <abs>/<file name>.
    """
    if not flags:
        return flags
    root = str(repo_root.resolve()).replace("\\", "/").rstrip("/")

    def scrub(path: str) -> str:
        normalized = path.replace("\\", "/")
        fold = os.name == "nt" or re.match(r"^[A-Za-z]:/", normalized) is not None
        candidate, base = (normalized.lower(), root.lower()) if fold else (normalized, root)
        if candidate == base or candidate.startswith(base + "/"):
            return "<source>" + normalized[len(root) :]
        return "<abs>/" + PurePath(normalized.rstrip("/")).name

    flags = _QUOTED_PATH.sub(lambda m: m.group(1) + scrub(m.group(2)) + m.group(1), flags)
    return _BARE_PATH.sub(lambda m: m.group("pre") + scrub(m.group("path")), flags)


def build_provenance(binary: Path, repo_root: Path) -> dict:
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
    # Flags can embed absolute paths (the PGO presets' -fprofile-instr-use=${sourceDir}/...).
    build["cxx_flags"] = hide_absolute_paths(cache.get("CMAKE_CXX_FLAGS"), repo_root)
    if build["build_type"]:
        build["cxx_flags_config"] = hide_absolute_paths(
            cache.get(f"CMAKE_CXX_FLAGS_{build['build_type'].upper()}"), repo_root
        )
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


def is_truthy_flag_value(value: str) -> bool:
    """Google Benchmark's IsTruthyFlagValue (src/commandlineflags.cc).

    One character is true when alphanumeric and not 0/f/F/n/N; a longer value is true unless it is
    false/no/off in any case; an empty value is true.
    """
    if len(value) == 1:
        return value.isascii() and value.isalnum() and value not in "0fFnN"
    if value:
        return value.lower() not in ("false", "no", "off")
    return True


def report_aggregates_only(args: list[str], environ: Mapping[str, str]) -> bool:
    """The --benchmark_report_aggregates_only setting Google Benchmark ends up with.

    The default comes from the BENCHMARK_REPORT_AGGREGATES_ONLY environment variable (else false);
    then each --benchmark_report_aggregates_only[=value] argument applies in order, the last one
    winning, and a bare flag is true. Kept in step with Get-EffectiveReportAggregatesOnly in
    tools/bench.ps1.
    """
    flag = "--benchmark_report_aggregates_only"
    env_value = environ.get("BENCHMARK_REPORT_AGGREGATES_ONLY")
    value = is_truthy_flag_value(env_value) if env_value is not None else False
    for arg in args:
        if arg == flag:
            value = True
        elif arg.startswith(flag + "="):
            value = is_truthy_flag_value(arg[len(flag) + 1 :])
    return value


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

    aggregates_only = report_aggregates_only(bench_args, os.environ)
    manifest = {
        "schema_version": 1,
        "generator": options.generator,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "preset": options.preset,
        "result_file": options.result.name,
        "exit_code": options.exit_code,
        "git": git_provenance(options.repo_root),
        "binary": {"name": options.binary.name, "sha256": sha256_of(options.binary)},
        "build": build_provenance(options.binary, options.repo_root),
        "benchmark": {
            "args": recorded_args(bench_args),
            "raw_repetitions": not aggregates_only,
            "report_aggregates_only": aggregates_only,
        },
        "machine": machine_class(),
    }
    options.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
