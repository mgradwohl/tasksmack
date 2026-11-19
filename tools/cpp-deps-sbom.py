#!/usr/bin/env python3
"""
tools/cpp-deps-sbom.py -- The C/C++ libraries in TaskSmack's binaries, for the source SBOM and OSV (#4438).

Usage:
    python -I tools/cpp-deps-sbom.py list  [--json] [--root DIR]
    python -I tools/cpp-deps-sbom.py merge --sbom FILE [--output FILE] [--root DIR]
    python -I tools/cpp-deps-sbom.py check --sbom FILE [--root DIR]
    python -I tools/cpp-deps-sbom.py osv   --sbom FILE

Syft (anchore/sbom-action) has no CMake or FetchContent cataloger, so the SPDX document it writes from the
source tree lists the GitHub Actions and the Python tooling but none of the libraries compiled into the app.
This script reads them from the one place they are pinned, the FetchContent_Declare() calls:

  cmake/Dependencies.cmake    compiled or linked into the shipped binaries
  tests/CMakeLists.txt        the test binaries only (marked TEST_DEPENDENCY_OF)
  benchmarks/CMakeLists.txt   the benchmark binaries only (marked DEV_DEPENDENCY_OF)

Each call must name the dependency, then GIT_REPOSITORY, then GIT_TAG with a full 40-hex commit, the shape
cmake/ThirdPartyNotices.cmake reads. A call in another shape, or one in a CMake file not listed above, fails
every subcommand, so a new dependency can't be left out. Licences come from ThirdPartyNotices.cmake's
_TS_NOTICE_LICENSES (the shipped ones) and TEST_ONLY_LICENSES below. The Linux build also links the
distribution's libbpf, libsystemd and libcurl, found with pkg_check_modules() in CMakeLists.txt: they are
listed with no version and no purl, since the version is whatever the installed distribution package has.

  list   prints the components (a table, or the SPDX packages with --json).
  merge  adds them to an SPDX-JSON document (replacing any this script added before), each with its version,
         its download location at the pinned commit, a pkg:github purl at that commit and its licence, and a
         relationship to the document's root package. In place unless --output is given.
  check  fails unless the document lists every pinned dependency at its pinned commit, and nothing stale.
  osv    asks OSV (https://api.osv.dev/v1/querybatch) about each listed component's pinned commit, which
         is how OSV indexes C/C++ projects (GIT ranges), and fails when any is affected by a known
         vulnerability or the query can't be made. A vulnerability in osv-scanner.toml's [[IgnoredVulns]]
         (the file osv-scanner itself reads), with a reason and an ignoreUntil date not yet passed, is
         reported but doesn't fail; once the date passes it fails again.

Exits 1 on any failure, 0 otherwise. `--root` points at another tree, for testing. Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
import time
import tomllib
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Which binaries each file's dependencies end up in.
SCOPES = {
    "cmake/Dependencies.cmake": "shipped",
    "tests/CMakeLists.txt": "test",
    "benchmarks/CMakeLists.txt": "benchmark",
}
NOTICES = "cmake/ThirdPartyNotices.cmake"
SYSTEM_LIBRARIES_FILE = "CMakeLists.txt"

# The test and benchmark dependencies are not in THIRD_PARTY_NOTICES.txt (they don't ship), so their licences
# are kept here. One missing from this table is listed as NOASSERTION, not failed: it ships nowhere.
TEST_ONLY_LICENSES = {
    "googletest": "BSD-3-Clause",
    "rapidcheck": "BSD-2-Clause",
    "benchmark": "Apache-2.0",
}

SPDXID_PREFIX = "SPDXRef-CppDep-"
RELATIONSHIPS = {
    "shipped": "STATIC_LINK",  # <root> STATIC_LINK <dep>: compiled into the app
    "system": "DYNAMIC_LINK",  # <root> DYNAMIC_LINK <dep>: the distribution's shared library
    "test": "TEST_DEPENDENCY_OF",  # <dep> TEST_DEPENDENCY_OF <root>
    "benchmark": "DEV_DEPENDENCY_OF",  # <dep> DEV_DEPENDENCY_OF <root>
}
OSV_URL = "https://api.osv.dev/v1/querybatch"
OSV_CONFIG = "osv-scanner.toml"

_CALL = re.compile(r"^[ \t]*FetchContent_Declare[ \t]*\(", re.MULTILINE)
_SHA = re.compile(r"[0-9a-f]{40}")
_GITHUB = re.compile(r"https://github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(?:\.git)?/?")
_SPDX_EXPRESSION = re.compile(r"[A-Za-z0-9.+-]+(?: (?:OR|AND|WITH) [A-Za-z0-9.+-]+)*")


class DependencyError(Exception):
    pass


@dataclass(frozen=True)
class Dependency:
    name: str
    scope: str  # shipped, test, benchmark or system
    repository: str = ""
    commit: str = ""
    version: str = ""  # the pin comment's tag, or the commit when the pin has no tag
    note: str = ""  # the pin comment, when it says more than the tag
    license: str = "NOASSERTION"
    license_text: str = ""  # the notice's wording, when it isn't a plain SPDX expression
    source: str = ""

    @property
    def spdxid(self) -> str:
        return SPDXID_PREFIX + re.sub(r"[^A-Za-z0-9.-]", "-", self.name)

    @property
    def purl(self) -> str:
        if not self.commit:
            return ""
        match = _GITHUB.fullmatch(self.repository)
        if match:
            return f"pkg:github/{match.group(1).lower()}/{match.group(2).lower()}@{self.commit}"
        return f"pkg:generic/{self.name.lower()}@{self.commit}?download_url={self.repository}"

    @property
    def download_location(self) -> str:
        return f"git+{self.repository}@{self.commit}" if self.commit else "NOASSERTION"


def _strip_comment(line: str) -> tuple[str, str]:
    code, _, comment = line.partition("#")
    return code.strip(), comment.strip()


def _calls(text: str) -> list[list[str]]:
    """Each FetchContent_Declare() call's lines, from the opening line to the one closing it."""
    calls = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not _CALL.match(line):
            continue
        body = [line[_CALL.match(line).end():]]
        for following in lines[index + 1:]:
            code, _ = _strip_comment(following)
            if code == ")":
                break
            body.append(following)
        else:
            raise DependencyError(f"a FetchContent_Declare() call is never closed: {line.strip()}")
        calls.append(body)
    return calls


def _version(comment: str, commit: str) -> tuple[str, str]:
    """The pin comment's tag ('v1.17.0 - pinned to SHA ...'), or the commit when it names none."""
    label = re.split(r"\s+-\s+[Pp]inned|^[Pp]inned", comment, maxsplit=1)[0].strip()
    if label and " " not in label and "(" not in label:
        return label, ""
    return commit, label


def _spdx_license(notice: str) -> tuple[str, str]:
    """A notice licence as an SPDX expression, and the notice's own words when they say more."""
    if _SPDX_EXPRESSION.fullmatch(notice):
        return notice, ""
    # Drop the prose in parentheses ("(used under the FTL)", "(the generator)"), keep groupings of identifiers.
    expression = re.sub(r"\s*\((?![A-Za-z0-9.+-]+(?: (?:OR|AND|WITH) [A-Za-z0-9.+-]+)+\))[^()]*\)", "", notice)
    expression = re.sub(r",?\s+and\s+", " AND ", expression).strip()
    tokens = re.sub(r"[()]", " ", expression).split()
    if tokens and all(_SPDX_EXPRESSION.fullmatch(token) for token in tokens) and expression.count("(") == expression.count(")"):
        return expression, notice
    return "NOASSERTION", notice


def _notice_licenses(root: Path) -> dict[str, str]:
    text = (root / NOTICES).read_text(encoding="utf-8")
    block = re.search(r"set\(_TS_NOTICE_LICENSES((?:\s+\"[^\"]*\")+)\)", text)
    if not block:
        raise DependencyError(f"{NOTICES} has no set(_TS_NOTICE_LICENSES ...) list")
    return dict(entry.split("|", 1) for entry in re.findall(r"\"([^\"]*)\"", block.group(1)))


def _cmake_files(root: Path) -> list[Path]:
    found = []
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = sorted(name for name in subdirectories
                                   if not name.startswith((".", "build", "_deps", "out")) and name != "node_modules")
        found += [Path(directory) / name for name in sorted(files) if name == "CMakeLists.txt" or name.endswith(".cmake")]
    return found


def fetchcontent_dependencies(root: Path = REPO) -> list[Dependency]:
    for path in _cmake_files(root):
        relative = path.relative_to(root).as_posix()
        if relative not in SCOPES and _CALL.search(path.read_text(encoding="utf-8", errors="replace")):
            raise DependencyError(f"{relative} calls FetchContent_Declare(), but this script reads only "
                                  f"{', '.join(SCOPES)}: add it to SCOPES in tools/cpp-deps-sbom.py")
    licenses = _notice_licenses(root)
    dependencies = []
    for relative, scope in SCOPES.items():
        path = root / relative
        if not path.is_file():
            continue
        for body in _calls(path.read_text(encoding="utf-8")):
            code_lines = [(code, comment) for code, comment in map(_strip_comment, body) if code]
            if len(code_lines) < 3 or not re.fullmatch(r"[A-Za-z0-9_]+", code_lines[0][0]):
                raise DependencyError(f"{relative}: a FetchContent_Declare() doesn't start with the dependency's name "
                                      f"on its own line: {body[0].strip() or body[1].strip()}")
            name = code_lines[0][0]
            repository = re.fullmatch(r"GIT_REPOSITORY\s+(\S+)", code_lines[1][0])
            tag = re.fullmatch(r"GIT_TAG\s+(\S+)", code_lines[2][0])
            if not repository or not tag or not _SHA.fullmatch(tag.group(1)):
                raise DependencyError(f"{relative}: {name}'s FetchContent_Declare() isn't the name, then GIT_REPOSITORY, "
                                      "then GIT_TAG with a 40-hex commit, one per line; give it that shape or teach "
                                      "tools/cpp-deps-sbom.py (and cmake/ThirdPartyNotices.cmake) to read it")
            commit = tag.group(1)
            version, note = _version(code_lines[2][1], commit)
            if scope == "shipped":
                if name not in licenses:
                    raise DependencyError(f"{name} ships but has no licence in {NOTICES}'s _TS_NOTICE_LICENSES")
                license, license_text = _spdx_license(licenses[name])
            else:
                license, license_text = TEST_ONLY_LICENSES.get(name, "NOASSERTION"), ""
            dependencies.append(Dependency(name, scope, repository.group(1), commit, version, note, license,
                                           license_text, relative))
    if not dependencies:
        raise DependencyError(f"no FetchContent_Declare() found in {', '.join(SCOPES)}")
    return dependencies


def system_libraries(root: Path = REPO) -> list[Dependency]:
    """The Linux build's pkg-config libraries: pkg_check_modules(<var> ... IMPORTED_TARGET <module>[>=version])."""
    path = root / SYSTEM_LIBRARIES_FILE
    if not path.is_file():
        return []
    modules = re.findall(r"pkg_check_modules\(\s*\w+[^)]*?\bIMPORTED_TARGET\s+([A-Za-z0-9_.+-]+)((?:[<>=]+[^\s)]+)?)",
                         path.read_text(encoding="utf-8"))
    return [Dependency(module, "system", note=f"Linux only: the distribution's shared library{f' ({need})' if need else ''}, "
                                              "at whatever version its package installs", source=SYSTEM_LIBRARIES_FILE)
            for module, need in dict.fromkeys(modules)]


def components(root: Path = REPO) -> list[Dependency]:
    return fetchcontent_dependencies(root) + system_libraries(root)


def spdx_package(dependency: Dependency) -> dict[str, object]:
    comment = {
        "shipped": "Compiled or linked into TaskSmack's shipped binaries",
        "test": "Test binaries only; not in any shipped package",
        "benchmark": "Benchmark binaries only; not in any shipped package",
        "system": "Dynamically linked; not shipped in TaskSmack's packages",
    }[dependency.scope]
    comment += f" (declared in {dependency.source})."
    if dependency.note:
        comment += f" {dependency.note[0].upper()}{dependency.note[1:]}."
    package: dict[str, object] = {
        "name": dependency.name,
        "SPDXID": dependency.spdxid,
        "versionInfo": dependency.version or "NOASSERTION",
        "supplier": "NOASSERTION",
        "downloadLocation": dependency.download_location,
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": dependency.license,
        "copyrightText": "NOASSERTION",
        "primaryPackagePurpose": "LIBRARY",
        "comment": comment,
    }
    if dependency.license_text:
        package["licenseComments"] = f"THIRD_PARTY_NOTICES.txt: {dependency.license_text}"
    if dependency.repository:
        package["homepage"] = re.sub(r"\.git$", "", dependency.repository)
    if dependency.purl:
        package["externalRefs"] = [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                    "referenceLocator": dependency.purl}]
    return package


def _root_package(document: dict) -> str:
    for relationship in document.get("relationships", []):
        if relationship.get("spdxElementId") == "SPDXRef-DOCUMENT" and relationship.get("relationshipType") == "DESCRIBES":
            return relationship["relatedSpdxElement"]
    described = document.get("documentDescribes") or []
    return described[0] if described else "SPDXRef-DOCUMENT"


def merge(document: dict, dependencies: list[Dependency]) -> dict:
    ours = lambda spdxid: str(spdxid).startswith(SPDXID_PREFIX)
    document["packages"] = [package for package in document.get("packages", []) if not ours(package.get("SPDXID"))]
    document["relationships"] = [relationship for relationship in document.get("relationships", [])
                                 if not ours(relationship.get("spdxElementId")) and not ours(relationship.get("relatedSpdxElement"))]
    root = _root_package(document)
    for dependency in dependencies:
        document["packages"].append(spdx_package(dependency))
        kind = RELATIONSHIPS[dependency.scope]
        if kind.endswith("_OF"):
            document["relationships"].append({"spdxElementId": dependency.spdxid, "relationshipType": kind,
                                              "relatedSpdxElement": root})
        else:
            document["relationships"].append({"spdxElementId": root, "relationshipType": kind,
                                              "relatedSpdxElement": dependency.spdxid})
    return document


def _purls(package: dict) -> list[str]:
    return [ref.get("referenceLocator", "") for ref in package.get("externalRefs", []) if ref.get("referenceType") == "purl"]


def check(document: dict, dependencies: list[Dependency]) -> list[str]:
    problems = []
    listed = {package.get("SPDXID"): package for package in document.get("packages", [])}
    for dependency in dependencies:
        package = listed.get(dependency.spdxid)
        if package is None:
            problems.append(f"{dependency.name} ({dependency.source}) is missing from the SBOM")
        elif package.get("name") != dependency.name:
            problems.append(f"{dependency.spdxid} is named {package.get('name')!r}, not {dependency.name!r}")
        elif dependency.purl and dependency.purl not in _purls(package):
            problems.append(f"{dependency.name} isn't listed at its pinned commit {dependency.commit} "
                            f"(the SBOM has {', '.join(_purls(package)) or 'no purl'})")
    expected = {dependency.spdxid for dependency in dependencies}
    for spdxid in listed:
        if str(spdxid).startswith(SPDXID_PREFIX) and spdxid not in expected:
            problems.append(f"the SBOM lists {listed[spdxid].get('name')} ({spdxid}), which no CMake file declares")
    return problems


def _post_json(url: str, payload: dict) -> dict:
    request = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"),
                                     headers={"Content-Type": "application/json"}, method="POST")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:  # noqa: S310 (a fixed https URL)
                return json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError) as error:
            if attempt == 2:
                raise DependencyError(f"OSV query failed: {error}") from error
            time.sleep(5 * (attempt + 1))
    raise AssertionError("unreachable")


def osv_ignores(root: Path = REPO, today: datetime.date | None = None) -> dict[str, str]:
    """The [[IgnoredVulns]] in osv-scanner.toml still in force: id -> reason. One with no reason or no
    ignoreUntil date is an error, so nothing is ignored for ever without a word."""
    path = root / OSV_CONFIG
    if not path.is_file():
        return {}
    today = today or datetime.date.today()
    ignores = {}
    for entry in tomllib.loads(path.read_text(encoding="utf-8")).get("IgnoredVulns", []):
        vuln, reason, until = entry.get("id"), str(entry.get("reason", "")).strip(), entry.get("ignoreUntil")
        if not vuln or not reason or until is None:
            raise DependencyError(f"{OSV_CONFIG}: every [[IgnoredVulns]] needs an id, a reason and an ignoreUntil date")
        if isinstance(until, datetime.datetime):
            until = until.date()
        if not isinstance(until, datetime.date):
            raise DependencyError(f"{OSV_CONFIG}: {vuln}'s ignoreUntil isn't a date")
        if until >= today:
            ignores[vuln] = f"{reason} (ignored until {until.isoformat()})"
    return ignores


def osv_scan(document: dict, post: Callable[[str, dict], dict] = _post_json,
             ignores: dict[str, str] | None = None) -> tuple[list[str], list[str]]:
    """(lines to print, vulnerabilities found) for every pinned component this script added to the document."""
    ignores = ignores or {}
    pinned = []
    for package in document.get("packages", []):
        if not str(package.get("SPDXID")).startswith(SPDXID_PREFIX):
            continue
        for purl in _purls(package):
            commit = purl.rpartition("@")[2].partition("?")[0]
            if _SHA.fullmatch(commit):
                pinned.append((package["name"], commit))
    if not pinned:
        raise DependencyError("the SBOM lists no pinned C/C++ dependency to scan; run `merge` first")
    results = post(OSV_URL, {"queries": [{"commit": commit} for _name, commit in pinned]}).get("results", [])
    if len(results) != len(pinned):
        raise DependencyError(f"OSV answered {len(results)} of {len(pinned)} queries")
    lines, found = [], []
    for (name, commit), result in zip(pinned, results):
        ids = [vuln.get("id", "?") for vuln in result.get("vulns", [])]
        if result.get("next_page_token"):
            ids.append("(more on another page)")
        failing = [vuln for vuln in ids if vuln not in ignores]
        found += [f"{name}@{commit}: {vuln}" for vuln in failing]
        status = ", ".join(failing) if failing else ("only ignored vulnerabilities" if ids else "no known vulnerability")
        lines.append(f"{name} {commit}: {status}")
        lines += [f"  {vuln} ignored: {ignores[vuln]}" for vuln in ids if vuln in ignores]
    return lines, found


def _read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip(),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("list", "merge", "check", "osv"))
    parser.add_argument("--sbom", type=Path, help="the SPDX-JSON document (merge, check, osv)")
    parser.add_argument("--output", type=Path, help="merge: write here instead of over --sbom")
    parser.add_argument("--json", action="store_true", help="list: print the SPDX packages")
    parser.add_argument("--root", type=Path, default=REPO, help="the source tree (default: this checkout)")
    args = parser.parse_args(argv)
    if args.command != "list" and args.sbom is None:
        parser.error(f"{args.command} needs --sbom")
    try:
        if args.command == "osv":
            lines, found = osv_scan(_read(args.sbom), ignores=osv_ignores(args.root))
            print("\n".join(lines))
            if found:
                print(f"{len(found)} known vulnerabilities in the pinned C/C++ dependencies:", file=sys.stderr)
                print("\n".join(found), file=sys.stderr)
                return 1
            return 0
        dependencies = components(args.root)
        if args.command == "list":
            if args.json:
                print(json.dumps([spdx_package(dependency) for dependency in dependencies], indent=2))
            else:
                for dependency in dependencies:
                    print(f"{dependency.name:<14} {dependency.scope:<10} {dependency.version or '-':<22} "
                          f"{dependency.license:<45} {dependency.purl or '-'}")
            return 0
        document = _read(args.sbom)
        if args.command == "merge":
            output = args.output or args.sbom
            output.write_text(json.dumps(merge(document, dependencies), indent=2) + "\n", encoding="utf-8")
            print(f"Added {len(dependencies)} C/C++ components to {output}")
            return 0
        problems = check(document, dependencies)
        if problems:
            print(f"{args.sbom} doesn't list the C/C++ dependencies as declared:", file=sys.stderr)
            print("\n".join(f"  {problem}" for problem in problems), file=sys.stderr)
            return 1
        print(f"{args.sbom} lists all {len(dependencies)} C/C++ components at their pinned commits.")
        return 0
    except (DependencyError, OSError, json.JSONDecodeError, tomllib.TOMLDecodeError) as error:
        print(f"cpp-deps-sbom: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
