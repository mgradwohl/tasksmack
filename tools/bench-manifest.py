#!/usr/bin/env python3
"""Write the provenance manifest that tools/bench.sh puts next to each benchmark result (#1424).

The field names match tools/bench.ps1's manifest, so one consumer can read both. Only an
anonymized machine class is recorded (CPU model, logical core count, OS): never the host name,
user name, other processes or command lines; the benchmark arguments are recorded as written
only when they are allowlisted options with safe values (hashed otherwise), and the compiler
flags are recorded only as SHA-256 hashes.

Usage:
    bench-manifest.py --preset-stem P                   (the preset part of the output file names)
    bench-manifest.py --manifest OUT --result RESULT_JSON --binary BIN --preset P \
        --repo-root DIR -- <benchmark args...>          (snapshot before the run, exit_code null)
    bench-manifest.py --manifest OUT --finalize --exit-code N   (after the run)
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
import socket
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path, PurePath


def git_provenance(repo_root: Path, user: str | None = None, hosts: list[str] | tuple[str, ...] = ()) -> dict:
    """The checkout's commit, branch and dirty flag; a user or host name in the branch is hidden
    (hide_name_identity)."""
    def git(*args: str) -> str | None:
        try:
            result = subprocess.run(
                ["git", "-C", str(repo_root), *args],
                capture_output=True,
                encoding="utf-8",  # git writes UTF-8 ref names, whatever the locale
                errors="replace",
                check=False,
            )
        except OSError:
            return None
        return result.stdout.strip() if result.returncode == 0 else None

    unknown = {"commit": None, "branch": None, "dirty": None}
    # Only the script's own checkout counts: git searches parent directories, so a source archive
    # unpacked inside another checkout would otherwise report that checkout's commit (#1445
    # review). The repository root must be git's top level (an empty prefix).
    if git("rev-parse", "--show-prefix") != "":
        return unknown
    commit = git("rev-parse", "HEAD")
    if commit is None:
        return unknown
    # Tracked changes only: untracked scratch files do not change what was built.
    status = git("status", "--porcelain", "--untracked-files=no")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    return {
        "commit": commit,
        "branch": None if branch is None else hide_name_identity(branch, user, hosts),
        "dirty": None if status is None else bool(status),
    }


def host_names() -> list[str]:
    """This machine's names for hide_identity(): the host name, its short form and the FQDN where
    known, longest first (so the FQDN is replaced before its short name)."""
    names = {socket.gethostname(), platform.node()}
    try:
        names.add(socket.getfqdn())
    except OSError:
        pass
    names |= {name.split(".")[0] for name in names if name}
    return sorted((name for name in names if name), key=len, reverse=True)


def home_prefixes(homes) -> list[str]:
    """The home-directory prefixes hide_identity() replaces, in both slash forms, longest first.

    Trailing separators are trimmed; only an empty prefix (a root, "/" or "\\") and a bare drive
    ("C:") are left out, since they would hide every path. Any other home is kept whatever its
    length (a home of /ab too). Kept in step with Hide-Identity in tools/bench.ps1.
    """
    prefixes = set()
    for home in homes:
        home = (home or "").rstrip("\\/")
        if not home or re.fullmatch(r"[A-Za-z]:", home):
            continue
        prefixes.update({home, home.replace("\\", "/"), home.replace("/", "\\")})
    return sorted(prefixes, key=len, reverse=True)


def identity_strings() -> tuple[list[str], str | None, list[str]]:
    """This user's home directory (both slash forms), user name and host names, for
    hide_identity()."""
    homes = {str(Path.home())}
    for variable in ("HOME", "USERPROFILE"):
        if os.environ.get(variable):
            homes.add(os.environ[variable])
    try:
        user = getpass.getuser()
    except (KeyError, OSError):
        user = os.environ.get("USER") or os.environ.get("USERNAME")
    return home_prefixes(homes), user, host_names()


# A user or host name only counts where it stands alone between separators (start or end,
# whitespace, a slash, a quote, '=', ':', ',' or ';'), so a user named "build" leaves -DBUILD=1
# and BUILD_TYPE alone but still hides -DBUILT_BY=build and C:/Users/build.
_SEPARATED = r"""\s/\\"'=:,;"""


def _hide_token(value: str, token: str | None, replacement: str, separated: str = _SEPARATED) -> str:
    if not token or len(token) < 3:
        return value
    pattern = rf"(?<![^{separated}]){re.escape(token)}(?![^{separated}])"
    return re.sub(pattern, replacement, value, flags=re.IGNORECASE)


# A name's own separators ('-', '.', '_', and '/' already in _SEPARATED) also bound a user or host
# name in a preset or a git branch: win-benchuser and feature/benchuser-fix must not keep it. Only
# these two names get the wider boundaries; the general identity pass keeps _SEPARATED, so an
# allowlisted --benchmark_filter value or x86_64 is never rewritten for a user named x86.
_NAME_SEPARATED = _SEPARATED + r"._\-"


def hide_name_identity(name: str, user: str | None, hosts: list[str] | tuple[str, ...] = ()) -> str:
    """A preset or git branch name with each host name (FQDN before short name) and the user name
    (3+ characters, any case) standing alone between _NAME_SEPARATED replaced by <host> / <user>
    (#1445 review): the preset names the output files and is recorded, and the branch is
    recorded, so neither may carry the user or the machine. Kept in step with Hide-NameIdentity in
    tools/bench.ps1."""
    for host in sorted(hosts, key=len, reverse=True):
        name = _hide_token(name, host, "<host>", _NAME_SEPARATED)
    return _hide_token(name, user, "<user>", _NAME_SEPARATED)


def preset_file_stem(component: str) -> str:
    """hide_name_identity() of a preset, for a file name: the placeholders without their angle
    brackets."""
    return component.replace("<", "").replace(">", "")


def hide_identity(value, prefixes: list[str], user: str | None, hosts: list[str] | tuple[str, ...] = ()):
    """Defensive last pass over every string in the manifest: any home-directory prefix
    (home_prefixes) becomes <home>, whatever its length; each host name (FQDN, short name) and
    the user name, when at least 3 characters and standing alone between separators, become
    <host> and <user>. Kept in step with Hide-Identity in tools/bench.ps1."""
    if isinstance(value, dict):
        return {key: hide_identity(item, prefixes, user, hosts) for key, item in value.items()}
    if isinstance(value, list):
        return [hide_identity(item, prefixes, user, hosts) for item in value]
    if not isinstance(value, str):
        return value
    for prefix in prefixes:
        value = re.sub(re.escape(prefix), "<home>", value, flags=re.IGNORECASE)
    for host in sorted(hosts, key=len, reverse=True):
        value = _hide_token(value, host, "<host>")
    return _hide_token(value, user, "<user>")


# The build information benchmarks/CMakeLists.txt writes next to the binary for each configuration
# (file(GENERATE)): the IPO that configuration of the TaskSmackBenchmarks target really builds with,
# including the IPO cmake/CompilerOptions.cmake turns on through a normal variable the cache does
# not show, and per configuration under multi-config generators. Read first (ipo_source
# "buildinfo"). Older build trees, without it, fall back to these CMakeCache.txt entries, best
# first. The same as $script:BuildInfoName / $script:IpoSources in tools/bench.ps1.
BUILDINFO_NAME = "TaskSmackBenchmarks.buildinfo.json"
IPO_SOURCES = ("CMAKE_INTERPROCEDURAL_OPTIMIZATION", "TASKSMACK_ENABLE_IPO")


def read_buildinfo(binary: Path) -> dict:
    """The build information next to the binary, as far as it is usable: "ipo" (ON or OFF), and
    "cxx_flags" -- the (cxx_flags_sha256, cxx_flags_config_sha256) pair, each a SHA-256 hex string
    or None (no such cache entry) -- when both keys are there and well-formed. Empty without a
    file. Kept in step with Read-BuildInfo in tools/bench.ps1."""
    try:
        info = json.loads((Path(os.path.abspath(binary)).parent / BUILDINFO_NAME).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return {}
    if not isinstance(info, dict):
        return {}
    usable = {}
    if info.get("ipo") in ("ON", "OFF"):
        usable["ipo"] = info["ipo"]

    def is_hash(value) -> bool:
        return value is None or (isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None)

    keys = ("cxx_flags_sha256", "cxx_flags_config_sha256")
    if all(key in info and is_hash(info[key]) for key in keys):
        usable["cxx_flags"] = tuple(info[key] for key in keys)
    return usable


# CMakeCache.txt entries as CMake itself reads them (cmState::ParseCacheEntry): "KEY":TYPE=VALUE,
# then KEY:TYPE=VALUE, then the untyped "KEY"=VALUE and KEY=VALUE. CMake quotes a key holding ':',
# and any other character -- '-', '.', '+' of a custom build type's CMAKE_CXX_FLAGS_<CONFIG> -- is
# part of an unquoted key. Trailing spaces, tabs and carriage returns are dropped, and a value in
# single quotes (how CMake writes one with trailing whitespace) loses them. Kept in step with
# Read-CMakeCache in tools/bench.ps1.
_CACHE_VALUE = r"(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$"
CACHE_ENTRY_PATTERNS = (
    re.compile(r'^"([^"]*)":[^=]*=' + _CACHE_VALUE),
    re.compile(r"^([^=:]*):[^=]*=" + _CACHE_VALUE),
    re.compile(r'^"([^"]*)"=' + _CACHE_VALUE),
    re.compile(r"^([^=]*)=" + _CACHE_VALUE),
)


def parse_cmake_cache(text: str) -> dict[str, str]:
    """The entries of a CMakeCache.txt, keyed case-sensitively; a later entry wins, as in CMake."""
    cache: dict[str, str] = {}
    for line in text.split("\n"):
        # One trailing carriage return is part of the line ending (cmSystemTools::GetLineFromStream).
        line = line.removesuffix("\r").lstrip(" \t")
        # Blank lines, '#' comments and '//' help text are not entries (cmCacheManager::LoadCache).
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        for pattern in CACHE_ENTRY_PATTERNS:
            if match := pattern.match(line):
                key, value = match.group(1), match.group(2)
                if len(value) >= 2 and value[0] == "'" and value[-1] == "'":
                    value = value[1:-1]
                cache[key] = value
                break
    return cache


def cmake_upper(value: str) -> str:
    """CMake's cmSystemTools::UpperCase: ASCII letters only, as the <CONFIG> suffix is built."""
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in value)


def find_build_tree(binary: Path, max_levels: int = 4) -> tuple[Path | None, str | None]:
    """The binary's build tree and multi-config configuration.

    The tree is the nearest ancestor holding CMakeCache.txt, at most max_levels directories up:
    build/<preset>/bin/ for a single-config generator, build/<preset>/bin/<Config>/ for a
    multi-config one (benchmarks/CMakeLists.txt). The configuration is that <Config> directory's
    name, or None for a flat bin/. Kept in step with Find-BuildTree in tools/bench.ps1.
    """
    binary_dir = Path(os.path.abspath(binary)).parent
    directory = binary_dir
    for _ in range(max_levels):
        if (directory / "CMakeCache.txt").is_file():
            parts = binary_dir.relative_to(directory).parts
            config = parts[1] if len(parts) == 2 and parts[0].lower() == "bin" else None
            return directory, config
        if directory.parent == directory:
            break
        directory = directory.parent
    return None, None


def build_provenance(binary: Path) -> dict:
    """Read build config from the CMakeCache.txt of the binary's build tree (build/<preset>)."""
    build: dict = {
        "build_type": None,
        "generator": None,
        "compiler": None,
        "compiler_id": None,
        "compiler_version": None,
        "cxx_flags_sha256": None,
        "cxx_flags_config_sha256": None,
        "cxx_flags_source": None,
        "ipo": None,
        "ipo_source": None,
    }
    buildinfo = read_buildinfo(binary)
    if "ipo" in buildinfo:
        build["ipo"], build["ipo_source"] = buildinfo["ipo"], "buildinfo"
    # The flag hashes the binary was linked with, from the build information (#1445 review): the
    # cache can have been reconfigured since.
    if "cxx_flags" in buildinfo:
        build["cxx_flags_sha256"], build["cxx_flags_config_sha256"] = buildinfo["cxx_flags"]
        build["cxx_flags_source"] = "buildinfo"
    build_dir, config = find_build_tree(binary)
    if build_dir is None:
        return build
    cache = parse_cmake_cache((build_dir / "CMakeCache.txt").read_bytes().decode("utf-8", errors="replace"))
    # A multi-config tree has no CMAKE_BUILD_TYPE: the binary's bin/<Config>/ names it.
    build["build_type"] = config or cache.get("CMAKE_BUILD_TYPE") or None
    build["generator"] = cache.get("CMAKE_GENERATOR")
    # Only the compiler's file name: its full path can sit under a user's home directory.
    if cache.get("CMAKE_CXX_COMPILER"):
        build["compiler"] = PurePath(cache["CMAKE_CXX_COMPILER"].replace("\\", "/")).name
    # The compiler flags are hashed, not recorded: two runs can be compared on them without the
    # manifest carrying their paths (include directories, the PGO presets' profile, prefix maps).
    # SHA-256 of the value as CMake reads it from CMakeCache.txt, UTF-8, otherwise unnormalized; null
    # when the entry is absent. The configuration's entry is named the way CMake names it. Only for a
    # tree without build information (older trees); benchmarks/CMakeLists.txt hashes the same way.
    if build["cxx_flags_source"] is None:
        build["cxx_flags_sha256"] = text_sha256(cache.get("CMAKE_CXX_FLAGS"))
        if build["build_type"]:
            build["cxx_flags_config_sha256"] = text_sha256(cache.get(f"CMAKE_CXX_FLAGS_{cmake_upper(build['build_type'])}"))
        build["cxx_flags_source"] = "cache"
    # Without build information (an older tree), interprocedural optimization from the cache, and
    # which entry said so (IPO_SOURCES).
    if build["ipo_source"] is None:
        for key in IPO_SOURCES:
            if key in cache:
                build["ipo"], build["ipo_source"] = cache[key], key
                break
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


def machine_label(machine: dict) -> str:
    """The one-line machine class, built from the other machine fields (Get-MachineLabel in
    tools/bench.ps1 builds the same string)."""
    cpu = machine.get("cpu_model") or "unknown CPU"
    os_version = machine.get("os_version") or ""
    return f"{cpu} / {machine.get('logical_cores')} logical cores / {machine.get('os_name')} {os_version}".rstrip()


def machine_class() -> dict:
    machine = {
        "label": None,
        "cpu_model": cpu_model(),
        "logical_cores": os.cpu_count(),
        "os_name": platform.system() or "unknown",
        "os_version": platform.release() or None,
        "arch": platform.machine() or None,
    }
    machine["label"] = machine_label(machine)
    return machine


# Manifest fields the identity pass leaves alone (#1445 review): validated, categorical values
# that cannot carry a user or host name but can coincide with one (a host named "Linux", a user
# named "clang"), and the flag hashes. Every other string is free-form input and is scrubbed: the
# compiler file name, the benchmark args, the git branch, the preset and result names, the CPU model.
# machine.os_version only goes through hide_name_identity() (a custom kernel's 6.8.0-benchhost), and
# machine.label is rebuilt from the scrubbed CPU model and OS version and the exempt fields. Numbers and booleans
# are never touched. build.build_type is exempt only as one of CMake's standard configurations
# (STANDARD_BUILD_TYPES): a custom configuration can be named after a user or host. The same
# lists as $script:IdentityExempt and $script:StandardBuildTypes in tools/bench.ps1.
IDENTITY_EXEMPT = frozenset(
    {
        "schema_version",
        "generator",
        "created_utc",
        "exit_code",
        "git.commit",
        "git.dirty",
        "binary.sha256",
        "build.generator",
        "build.compiler_id",
        "build.compiler_version",
        "build.ipo",
        "build.ipo_source",
        "build.cxx_flags_sha256",
        "build.cxx_flags_config_sha256",
        "build.cxx_flags_source",
        "benchmark.raw_repetitions",
        "benchmark.report_aggregates_only",
        "machine.label",
        "machine.logical_cores",
        "machine.os_name",
        "machine.os_version",
        "machine.arch",
    }
)


STANDARD_BUILD_TYPES = frozenset({"Debug", "Release", "RelWithDebInfo", "MinSizeRel"})


def hide_manifest_identity(manifest: dict, prefixes: list[str], user: str | None, hosts: list[str] | tuple[str, ...] = ()) -> dict:
    """hide_identity() over the manifest's free-form fields only (see IDENTITY_EXEMPT)."""

    def walk(value, path: str):
        if isinstance(value, dict):
            return {key: walk(item, f"{path}.{key}" if path else key) for key, item in value.items()}
        if path in IDENTITY_EXEMPT or (path == "build.build_type" and value in STANDARD_BUILD_TYPES):
            return value
        if path == "build.build_type" and isinstance(value, str):
            # A custom build type is a name like a preset (ASan-benchuser, Release_benchhost): its
            # own '-', '.' and '_' bound a user or host name too.
            value = hide_name_identity(value, user, hosts)
        return hide_identity(value, prefixes, user, hosts)

    result = walk(manifest, "")
    if isinstance(result.get("machine"), dict):
        # The OS version is exempt as a whole but not free of names: a Linux kernel built with
        # CONFIG_LOCALVERSION reports 6.8.0-benchhost. A user or host name in it, between its own
        # '-', '.' and '_', becomes <user> / <host> (hide_name_identity), and the label is rebuilt
        # from the result.
        if isinstance(result["machine"].get("os_version"), str):
            result["machine"]["os_version"] = hide_name_identity(result["machine"]["os_version"], user, hosts)
        result["machine"]["label"] = machine_label(result["machine"])
    return result


def text_sha256(value: str | None) -> str | None:
    """SHA-256 of a string's UTF-8 bytes, or None for no value. Get-TextSha256 in tools/bench.ps1."""
    return None if value is None else hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# The benchmark arguments (#1445 review). An argument is recorded as written only when it is a
# Google Benchmark option whose value is safe by construction (ALLOWED_ARGUMENTS: numbers,
# booleans, enumerations, and the --benchmark_filter regex, which the identity pass still covers);
# a value that fails its pattern, and every other argument (--benchmark_context=..., unknown ones),
# is recorded as <name>=sha256:<hex of the value>, or sha256:<hex of the argument> when it has no
# --name=value form. Runs stay comparable on their arguments without recording paths or other
# free text. The script's own --benchmark_out keeps its file name (extra ones are refused before
# launch). The same table as $script:AllowedArguments in tools/bench.ps1.
_NUMBER = r"[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?"
_BOOLEAN = r"(?i:true|false|yes|no|on|off|t|f|y|n|1|0)"
ALLOWED_ARGUMENTS = {
    "--benchmark_repetitions": r"[0-9]+",
    "--benchmark_min_time": _NUMBER + r"[sx]?",
    "--benchmark_min_warmup_time": _NUMBER + r"s?",
    "--benchmark_display_aggregates_only": _BOOLEAN,
    "--benchmark_report_aggregates_only": _BOOLEAN,
    "--benchmark_enable_random_interleaving": _BOOLEAN,
    "--benchmark_counters_tabular": _BOOLEAN,
    "--benchmark_dry_run": _BOOLEAN,
    "--benchmark_list_tests": _BOOLEAN,
    "--benchmark_time_unit": r"ns|us|ms|s",
    "--benchmark_format": r"console|json|csv",
    "--benchmark_out_format": r"console|json|csv",
    "--benchmark_color": r"(?i:auto|true|false|yes|no|on|off|1|0)",
    "--v": r"[0-9]+",
    "--benchmark_filter": r".*",
}


def record_argument(argument: str) -> str:
    """How one benchmark argument is recorded in the manifest (see ALLOWED_ARGUMENTS)."""
    if argument.startswith("--benchmark_out="):
        return "--benchmark_out=" + PurePath(argument.split("=", 1)[1].replace("\\", "/")).name
    match = re.fullmatch(r"(--[A-Za-z0-9_]+)(?:=(.*))?", argument, re.DOTALL)
    if match is None:
        return "sha256:" + text_sha256(argument)
    name, value = match.group(1), match.group(2)
    pattern = ALLOWED_ARGUMENTS.get(name)
    if value is None:
        # A bare flag: as written for a boolean option (Google Benchmark reads it as true).
        return argument if pattern == _BOOLEAN else "sha256:" + text_sha256(argument)
    if pattern is not None and re.fullmatch(f"(?:{pattern})", value, re.DOTALL):
        return argument
    return f"{name}=sha256:{text_sha256(value)}"


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
    parser.add_argument("--manifest", type=Path)
    # bench.sh names its output files from this (hide_name_identity), before anything is written.
    parser.add_argument("--preset-stem")
    parser.add_argument("--result", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--preset")
    # Without --exit-code this is the snapshot taken before the benchmark starts (exit_code
    # null); --finalize then only records the exit code in that manifest, so provenance describes
    # what was launched even if the checkout, build or binary changes during the run (#1445).
    parser.add_argument("--exit-code", type=int, default=None)
    parser.add_argument("--finalize", action="store_true")
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--generator", default="tools/bench.sh")
    parser.add_argument("bench_args", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    if options.preset_stem is not None:
        _, user, hosts = identity_strings()
        # UTF-8 bytes whatever stdout's encoding (a CP1252 console would raise on a non-ASCII
        # preset, and bench.sh captures this under set -e), and no newline: on Windows it would
        # reach bash's command substitution as "\r\n". The only output bench.sh captures.
        sys.stdout.flush()
        sys.stdout.buffer.write(preset_file_stem(hide_name_identity(options.preset_stem, user, hosts)).encode("utf-8"))
        sys.stdout.buffer.flush()
        return 0
    if options.manifest is None:
        parser.error("--manifest is required")
    if options.finalize:
        manifest = json.loads(options.manifest.read_text(encoding="utf-8"))
        manifest["exit_code"] = options.exit_code
        options.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        return 0
    missing = [name for name in ("result", "binary", "preset", "repo_root") if getattr(options, name) is None]
    if missing:
        parser.error("the snapshot needs " + ", ".join("--" + name.replace("_", "-") for name in missing))
    bench_args = options.bench_args
    if bench_args and bench_args[0] == "--":
        bench_args = bench_args[1:]

    aggregates_only = report_aggregates_only(bench_args, os.environ)
    prefixes, user, hosts = identity_strings()
    manifest = {
        "schema_version": 1,
        "generator": options.generator,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "preset": hide_name_identity(options.preset, user, hosts),
        "result_file": options.result.name,
        "exit_code": options.exit_code,
        "git": git_provenance(options.repo_root, user, hosts),
        "binary": {"name": options.binary.name, "sha256": sha256_of(options.binary)},
        "build": build_provenance(options.binary),
        "benchmark": {
            "args": [record_argument(argument) for argument in bench_args],
            "raw_repetitions": not aggregates_only,
            "report_aggregates_only": aggregates_only,
        },
        "machine": machine_class(),
    }
    manifest = hide_manifest_identity(manifest, prefixes, user, hosts)
    options.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
