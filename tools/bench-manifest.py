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
import socket
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path, PurePath


def git_provenance(repo_root: Path) -> dict:
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
#     each item scrubbed); a generic "-opt=" / "--opt=" (the value after the first '='); an MSVC
#     path switch (/I, /FI, /LIBPATH:, ...), always; or a dash joined switch (-I, -isystem, ...)
#     when what follows it is a path. Otherwise the whole argument is the operand.
#  3. An operand is a path when it starts with a drive (C:\ or C:/), a UNC or device path (\\,
#     //, \\?\, \\.\), a POSIX path of two or more segments (/home/u/x) or ~. A path inside the
#     checkout becomes <source>/relative, any other <abs>/<file name>; a relative operand
#     (/Iinclude/common's include/common) is kept. A ';' list is scrubbed item by item, and an
#     operand with a drive, UNC or device path inside it (FOO:C:/x) is scrubbed from there.
#  4. Re-join with single spaces, putting each argument's quote back before the piece it opened
#     on (or around the whole argument when that piece no longer exists).
#
# A leading-'/' argument is an MSVC option or a POSIX path, decided in this order:
#  a. It starts with an MSVC path switch (_MSVC_PATH_SWITCHES, case-sensitive): the switch is
#     peeled and what follows is the operand, kept unless it is itself absolute. /Iinclude/common
#     stays; /I/home/u/inc becomes /I<abs>/inc.
#  b. Otherwise it is a POSIX path only with two or more segments. Every other MSVC option
#     (/DWIN32, /U..., /W4, /O2, /EHsc, /std:c++latest, /Zc:..., /MD) is a single segment and
#     stays, and an option-looking path such as /Users/u/x (not a path switch: /U takes no path)
#     is still a path. An option with a path after '=' (/DDIR=/home/u/x) goes through the generic
#     "opt=" rule first.
_PREFIX_MAP_SWITCHES = ("-ffile-prefix-map=", "-fdebug-prefix-map=", "-fmacro-prefix-map=", "-fprofile-prefix-map=")
_LIST_SWITCHES = ("-Wl,", "-Wa,", "-Wp,")
# Longest first, case-sensitive (/FR is not /Fr).
_MSVC_PATH_SWITCHES = ("/external:I", "/LIBPATH:", "/FI", "/Fo", "/Fd", "/Fe", "/Fp", "/Fa", "/FR", "/Fr", "/I")
# Longest first, so -isystem is not read as -I... (case matters: -I is not -i).
_JOINED_SWITCHES = (
    "-iwithprefixbefore",
    "-iwithprefix",
    "-idirafter",
    "-isysroot",
    "-iprefix",
    "-imacros",
    "-isystem",
    "-include",
    "-iquote",
    "-imsvc",
    "-I",
    "-L",
    "-B",
    "-F",
)
_PATH_HEAD = re.compile(r"""^(?:[A-Za-z]:[\\/]|\\\\|//|/[^/\\]+/|~[^/\\]*(?:[/\\]|$))""")
_EMBEDDED_HEAD = re.compile(r"""[A-Za-z]:[\\/](?![\\/])|\\\\|(?<!:)//""")


def split_flag_arguments(flags: str) -> list[tuple[str, str | None, int]]:
    """Split a flag string into (text, first quote char or None, offset it opened at).

    Grouping quotes are removed from the text; everything else is kept verbatim, so the argument
    can be re-emitted as written. Backslashes are literal (Windows paths keep them), except before
    a double quote, where the CommandLineToArgvW parity rule applies: an odd run escapes the quote,
    which stays in the text as written (-DAPP_NAME=\\"TaskSmack\\"); an even run leaves it a
    grouping quote. One exception keeps Windows paths intact: inside a double-quoted group, a
    backslash and quote right before whitespace or the end close the group ("C:\\dir\\").
    Kept in step with Split-FlagArguments in tools/bench.ps1.
    """
    arguments = []
    index, length = 0, len(flags)
    while index < length:
        if flags[index].isspace():
            index += 1
            continue
        text, quote, quote_start, open_quote = "", None, 0, None
        while index < length:
            char = flags[index]
            if open_quote is None and char.isspace():
                break
            if char == "\\":
                end = index
                while end < length and flags[end] == "\\":
                    end += 1
                if end < length and flags[end] == '"' and (end - index) % 2 == 1:
                    after = flags[end + 1] if end + 1 < length else ""
                    if open_quote == '"' and (after == "" or after.isspace()):
                        text += flags[index:end]
                        open_quote = None
                    else:
                        text += flags[index : end + 1]
                    index = end + 1
                else:
                    text += flags[index:end]
                    index = end
                continue
            if open_quote is not None:
                if char == open_quote:
                    open_quote = None
                else:
                    text += char
                index += 1
                continue
            if char in "\"'":
                if quote is None:
                    quote, quote_start = char, len(text)
                open_quote = char
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
        # Compared in the root's canonical form (repo_root.resolve() above): realpath expands
        # Windows 8.3 short names (C:/Users/RUNNER~1/..., as TEMP is on GitHub's Windows runners)
        # and resolves links, so a checkout reached through another spelling still maps to
        # <source>. UNC and device paths are left alone (no network lookups). The file name kept
        # for a path outside the checkout is the one written in the flags.
        canonical = normalized
        if os.path.isabs(path) and not normalized.startswith("//"):
            canonical = os.path.realpath(path).replace("\\", "/")
        fold = os.name == "nt" or re.match(r"^[A-Za-z]:/", canonical) is not None
        candidate, base = (canonical.lower(), root.lower()) if fold else (canonical, root)
        if candidate == base or candidate.startswith(base + "/"):
            return "<source>" + canonical[len(root) :]
        return "<abs>/" + PurePath(normalized.rstrip("/")).name

    def operand(value: str) -> str:
        # A value wrapped in escaped quotes (-DDATA_DIR=\"/home/u/data\") keeps them around the
        # scrubbed path.
        if len(value) >= 4 and value.startswith('\\"') and value.endswith('\\"'):
            return '\\"' + operand(value[2:-2]) + '\\"'
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
        for switch in _MSVC_PATH_SWITCHES:
            if argument.startswith(switch) and len(argument) > len(switch):
                return [(0, switch), (len(switch), operand(argument[len(switch) :]))]
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


def identity_strings() -> tuple[list[str], str | None, list[str]]:
    """This user's home directory (both slash forms), user name and host names, for
    hide_identity()."""
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
    return sorted(prefixes, key=len, reverse=True), user, host_names()


# A user or host name only counts where it stands alone between separators (start or end,
# whitespace, a slash, a quote, '=', ':', ',' or ';'), so a user named "build" leaves -DBUILD=1
# and BUILD_TYPE alone but still hides -DBUILT_BY=build and C:/Users/build.
_SEPARATED = r"""\s/\\"'=:,;"""


def _hide_token(value: str, token: str | None, replacement: str) -> str:
    if not token or len(token) < 3:
        return value
    pattern = rf"(?<![^{_SEPARATED}]){re.escape(token)}(?![^{_SEPARATED}])"
    return re.sub(pattern, replacement, value, flags=re.IGNORECASE)


def hide_identity(value, prefixes: list[str], user: str | None, hosts: list[str] | tuple[str, ...] = ()):
    """Defensive last pass over every string in the manifest: any home-directory prefix becomes
    <home> (always); each host name (FQDN, short name) and the user name, when at least 3
    characters and standing alone between separators, become <host> and <user>. Kept in step
    with Hide-Identity in tools/bench.ps1."""
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
    build_dir, config = find_build_tree(binary)
    if build_dir is None:
        return build
    cache: dict[str, str] = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_]+)(?::[A-Za-z]+)?=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    # A multi-config tree has no CMAKE_BUILD_TYPE: the binary's bin/<Config>/ names it.
    build["build_type"] = config or cache.get("CMAKE_BUILD_TYPE") or None
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
# named "clang"). Every other string is free-form input and is scrubbed: the compiler file name
# and flags, the benchmark args, the git branch, the preset and result names, the CPU model.
# machine.label is rebuilt from the scrubbed CPU model and the exempt fields. Numbers and booleans
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
        return hide_identity(value, prefixes, user, hosts)

    result = walk(manifest, "")
    if isinstance(result.get("machine"), dict):
        result["machine"]["label"] = machine_label(result["machine"])
    return result


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
    manifest = hide_manifest_identity(manifest, *identity_strings())
    options.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
