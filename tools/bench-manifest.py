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
import getpass
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


# Absolute paths inside compiler flags (#1445 review). The flag string is split into arguments
# and each argument is scrubbed on its own; tools/bench.ps1 does the same step for step
# (Split-FlagArguments / Hide-AbsolutePaths), with the same switch lists.
#  1. Split like a shell, but with no backslash escapes (Windows paths keep their backslashes):
#     whitespace separates arguments, and "..." or '...' quotes a span that may hold spaces. Each
#     argument remembers its first quote character and where that quote opened.
#  2. Peel the switch: a prefix-map switch (its OLD=NEW value is split at the first '=', as
#     clang does, and each side scrubbed on its own); a comma-list switch (-Wl, and friends:
#     each item scrubbed); a generic "-opt=" / "--opt=" (the value after the first '='); or a
#     joined switch (-I, -isystem, /I, ...) when what follows it is a path. Otherwise the whole
#     argument is the operand.
#  3. An operand is a path when it starts with a drive (C:\ or C:/), a UNC or device path (\\,
#     //, \\?\, \\.\), a POSIX path of two or more segments (/home/u/x; so /DWIN32 stays) or ~.
#     A path inside the checkout becomes <source>/relative, any other <abs>/<file name>. A ';'
#     list is scrubbed item by item, and an operand with a drive, UNC or device path inside it
#     (FOO:C:/x) is scrubbed from there.
#  4. Re-join with single spaces, putting each argument's quote back before the piece it opened
#     on (or around the whole argument when that piece no longer exists).
_PREFIX_MAP_SWITCHES = ("-ffile-prefix-map=", "-fdebug-prefix-map=", "-fmacro-prefix-map=", "-fprofile-prefix-map=")
_LIST_SWITCHES = ("-Wl,", "-Wa,", "-Wp,")
# Longest first, so -isystem is not read as -I... (case matters: -I is not -i).
_JOINED_SWITCHES = (
    "-iwithprefixbefore",
    "-iwithprefix",
    "-idirafter",
    "/LIBPATH:",
    "-isysroot",
    "-iprefix",
    "-imacros",
    "-isystem",
    "-include",
    "-iquote",
    "-imsvc",
    "/FI",
    "/Fo",
    "/Fd",
    "/Fe",
    "/Fp",
    "-I",
    "-L",
    "-B",
    "-F",
    "/I",
)
_PATH_HEAD = re.compile(r"""^(?:[A-Za-z]:[\\/]|\\\\|//|/[^/\\]+/|~[^/\\]*(?:[/\\]|$))""")
_EMBEDDED_HEAD = re.compile(r"""[A-Za-z]:[\\/](?![\\/])|\\\\|(?<!:)//""")


def split_flag_arguments(flags: str) -> list[tuple[str, str | None, int]]:
    """Split a flag string into (unquoted text, first quote char or None, offset it opened at)."""
    arguments = []
    index, length = 0, len(flags)
    while index < length:
        if flags[index].isspace():
            index += 1
            continue
        text, quote, quote_start = "", None, 0
        while index < length and not flags[index].isspace():
            char = flags[index]
            if char in "\"'":
                close = flags.find(char, index + 1)
                close = length if close < 0 else close
                if quote is None:
                    quote, quote_start = char, len(text)
                text += flags[index + 1 : close]
                index = close + 1
            else:
                text += char
                index += 1
        arguments.append((text, quote, quote_start))
    return arguments


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

    def operand(value: str) -> str:
        if ";" in value:
            return ";".join(operand(item) for item in value.split(";"))
        if _PATH_HEAD.match(value):
            return scrub(value)
        if embedded := _EMBEDDED_HEAD.search(value):
            return value[: embedded.start()] + scrub(value[embedded.start() :])
        return value

    def pieces(argument: str) -> list[tuple[int, str]]:
        """(offset in the argument, scrubbed text) for each part of the argument."""
        for switch in _PREFIX_MAP_SWITCHES:
            if argument.startswith(switch):
                old, separator, new = argument[len(switch) :].partition("=")
                result = [(0, switch), (len(switch), operand(old))]
                if separator:
                    result += [(len(switch) + len(old), "="), (len(switch) + len(old) + 1, operand(new))]
                return result
        for switch in _LIST_SWITCHES:
            if argument.startswith(switch):
                result, offset = [(0, switch)], len(switch)
                for position, item in enumerate(argument[len(switch) :].split(",")):
                    if position:
                        result.append((offset, ","))
                        offset += 1
                    result.append((offset, operand(item)))
                    offset += len(item)
                return result
        if argument[:1] in ("-", "/") and "=" in argument:
            head = argument[: argument.index("=") + 1]
            if not re.search(r"[\\/]", head[1:]):
                return [(0, head), (len(head), operand(argument[len(head) :]))]
        for switch in _JOINED_SWITCHES:
            if argument.startswith(switch) and _PATH_HEAD.match(argument[len(switch) :]):
                return [(0, switch), (len(switch), operand(argument[len(switch) :]))]
        return [(0, operand(argument))]

    joined = []
    for text, quote, quote_start in split_flag_arguments(flags):
        parts = pieces(text)
        if quote is None:
            joined.append("".join(part for _, part in parts))
            continue
        starts = [start for start, _ in parts]
        at = starts.index(quote_start) if quote_start in starts else 0
        joined.append("".join(part for _, part in parts[:at]) + quote + "".join(part for _, part in parts[at:]) + quote)
    return " ".join(joined)


def identity_strings() -> tuple[list[str], str | None]:
    """This user's home directory (both slash forms) and user name, for hide_identity()."""
    homes = {str(Path.home())}
    for variable in ("HOME", "USERPROFILE"):
        if os.environ.get(variable):
            homes.add(os.environ[variable])
    prefixes = set()
    for home in homes:
        home = home.rstrip("\\/")
        if len(home) > 3:  # never a bare drive or "/"
            prefixes.update({home, home.replace("\\", "/"), home.replace("/", "\\")})
    try:
        user = getpass.getuser()
    except (KeyError, OSError):
        user = os.environ.get("USER") or os.environ.get("USERNAME")
    return sorted(prefixes, key=len, reverse=True), user


# The user name only counts where it stands alone between separators (start or end, whitespace,
# a slash, a quote, '=', ':', ',' or ';'), so a user named "build" leaves -DBUILD=1 and
# BUILD_TYPE alone but still hides -DBUILT_BY=build and C:/Users/build.
_SEPARATED = r"""\s/\\"'=:,;"""


def hide_identity(value, prefixes: list[str], user: str | None):
    """Defensive last pass over every string in the manifest: any home-directory prefix becomes
    <home> (always), and the user name, when it is at least 3 characters and stands alone
    between separators, becomes <user>."""
    if isinstance(value, dict):
        return {key: hide_identity(item, prefixes, user) for key, item in value.items()}
    if isinstance(value, list):
        return [hide_identity(item, prefixes, user) for item in value]
    if not isinstance(value, str):
        return value
    for prefix in prefixes:
        value = re.sub(re.escape(prefix), "<home>", value, flags=re.IGNORECASE)
    if user and len(user) >= 3:
        value = re.sub(
            rf"(?<![^{_SEPARATED}]){re.escape(user)}(?![^{_SEPARATED}])", "<user>", value, flags=re.IGNORECASE
        )
    return value


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
    # A reused build tree keeps CMakeFiles/<version>/ from every CMake that configured it: read the
    # one matching the cache's CMake version, and leave the compiler unknown rather than guess.
    if compiler_file := cmake_compiler_file(build_dir, cache):
        text = compiler_file.read_text(encoding="utf-8", errors="replace")
        if match := re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text):
            build["compiler_id"] = match.group(1)
        if match := re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text):
            build["compiler_version"] = match.group(1)
    return build


def cmake_compiler_file(build_dir: Path, cache: dict[str, str]) -> Path | None:
    """CMakeFiles/<major.minor.patch>/CMakeCXXCompiler.cmake for the CMake version in the cache.

    A development build of CMake names the directory with a suffix (4.1.20250101-gabc), so a
    single directory starting with the version followed by '-' is accepted too; anything else
    (no version in the cache, no match, or several) is None.
    """
    parts = [cache.get(f"CMAKE_CACHE_{part}_VERSION") for part in ("MAJOR", "MINOR", "PATCH")]
    if not all(parts):
        return None
    version = ".".join(parts)
    exact = build_dir / "CMakeFiles" / version / "CMakeCXXCompiler.cmake"
    if exact.is_file():
        return exact
    candidates = [
        path
        for path in (build_dir / "CMakeFiles").glob(f"{version}-*/CMakeCXXCompiler.cmake")
        if path.is_file()
    ]
    return candidates[0] if len(candidates) == 1 else None


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
    manifest = hide_identity(manifest, *identity_strings())
    options.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
