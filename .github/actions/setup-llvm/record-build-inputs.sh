#!/usr/bin/env bash
# Record the toolchain inputs of a Linux release build (#3393): bash record-build-inputs.sh OUT RELEASE [MAJOR]
#
# release.yml's build-linux and verify-reproducibility each write one after setup-llvm, and
# verify-reproducibility diffs the two. setup-llvm pins only the LLVM release (RELEASE, e.g. 23.1.5),
# not apt.llvm.org's snapshot inside it, so this file is where the exact build string lives: two
# builds on different snapshots show it here, beside any package digest difference it explains.
# Plain sorted "key: value" lines, so `diff -u` reads well; nothing in it is per-run (no times, no
# cache hits), so two builds from the same inputs write identical files.
set -euo pipefail

out="$1"
release="$2"
major="${3:-${release%%.*}}"

mkdir -p "$(dirname "$out")"
{
  echo "runner_image: ${ImageOS:-unknown} ${ImageVersion:-unknown}"
  # shellcheck disable=SC1091
  echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
  echo "architecture: $(dpkg --print-architecture)"
  echo "llvm_release: $release"
  echo "clang: $("clang-${major}" --version | head -n 1)"
  echo "ld.lld: $("ld.lld-${major}" --version | head -n 1)"
  echo "cmake: $(cmake --version | head -n 1)"
  echo "ninja: $(ninja --version)"
  echo "python3: $(python3 --version 2>&1)"
  # Every installed package of that release's apt.llvm.org build, with its exact build string
  # (1:<release>~++<date>+<commit>-1~exp1~<build>).
  dpkg-query -W -f='${db:Status-Abbrev}${Package}=${Version}\n' |
    awk -v prefix="=1:${release}~" '/^ii/ && index($0, prefix) { sub(/^ii +/, ""); print "llvm_package: " $0 }' |
    LC_ALL=C sort
} > "$out"
cat "$out"
