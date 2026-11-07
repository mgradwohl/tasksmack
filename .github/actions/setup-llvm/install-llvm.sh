#!/usr/bin/env bash
# Install LLVM <major> and the build's system packages on an Ubuntu runner (setup-llvm, #3393).
#
# Environment (from action.yml): LLVM_VERSION (major), EXPECT_RELEASE (e.g. 23.1.5, or empty),
# CACHE_DIR / CACHE_RESTORED (the LLVM package cache, expect-release only), INSTALL_* (the action's
# optional tools). Writes cache-hit and staged to $GITHUB_OUTPUT.
#
# Two paths:
#   - Cache hit: every LLVM .deb this job needs was restored into CACHE_DIR, so they are installed
#     from there and apt.llvm.org is never contacted. apt-get still pulls their Ubuntu dependencies
#     and the system packages from the Ubuntu archive.
#   - Otherwise: apt.llvm.org's llvm.sh adds the repository and installs clang, then apt-get installs
#     the rest. Every network step retries with backoff, since apt.llvm.org's key, installer and
#     repository have each failed transiently on GitHub's runners (2026-11-03: "Could not resolve
#     host: apt.llvm.org" for hours). With EXPECT_RELEASE set, the installed .debs of that release
#     are then staged in CACHE_DIR for the caller to save.
set -euo pipefail

LLVM_VER="${LLVM_VERSION:-}"
# Restored after removing this as "provably unreachable": GitHub Copilot's own
# code-review setup-step runner invokes this action through a path that does not
# resolve `${{ env.LLVM_VERSION }}` the way a normal Actions runner does, sending an
# empty string here in practice (reproduced directly -- see PR #859's review thread).
# Every real `uses:` call site in this repo's own workflows still passes a value, but
# that external caller doesn't, so the fallback stays.
if [[ -z "$LLVM_VER" ]]; then
  LLVM_VER='23'
fi
if [[ ! "$LLVM_VER" =~ ^[0-9]+$ ]]; then
  echo "llvm-version must be a numeric major version" >&2
  exit 1
fi
EXPECT_RELEASE="${EXPECT_RELEASE:-}"
CACHE_DIR="${CACHE_DIR:-}"
CACHE_RESTORED="${CACHE_RESTORED:-false}"
OUTPUT="${GITHUB_OUTPUT:-/dev/null}"

# Runs "$@" up to 4 times, waiting 15, 30 and 60 seconds between tries (about 2 minutes in all,
# plus each try's own timeout). Bounded, so a real outage still fails the job instead of hanging it.
retry() {
  local attempt=1 delay=15 status
  while true; do
    if "$@"; then
      return 0
    else
      status=$?
    fi
    if (( attempt >= 4 )); then
      echo "::error::'$*' failed $attempt times (last exit $status)."
      return "$status"
    fi
    echo "::warning::'$*' failed (exit $status), try $attempt of 4; retrying in ${delay}s."
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done
}

# GitHub's Ubuntu mirror list prefers azure.archive.ubuntu.com, which can
# accept a connection and then stall indefinitely. Select the standard
# archive before any apt operation so a hung Azure request cannot prevent
# fallback logic from ever running. Acquire::Retries also covers the
# apt-get calls inside llvm.sh, which this script can't pass options to.
sudo tee /etc/apt/apt.conf.d/80-tasksmack-network >/dev/null <<'APT_CONFIG'
Acquire::Retries "5";
Acquire::http::Timeout "15";
Acquire::https::Timeout "15";
APT_CONFIG
# x64 only: arm64 packages live on ports.ubuntu.com, not archive.ubuntu.com, so the
# ubuntu-26.04-arm image's mirror list is left as it is (#2214).
if [[ -f /etc/apt/apt-mirrors.txt && "$(dpkg --print-architecture)" == 'amd64' ]]; then
  echo 'https://archive.ubuntu.com/ubuntu/' |
    sudo tee /etc/apt/apt-mirrors.txt >/dev/null
fi
sudo sed -i \
  's|https\?://azure.archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
  /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || true

# Always include GUI dependencies; libcurl for the update check's HTTPS client (#3238); libbpf for
# the helper's UDP byte counter (#3366, libbpf required on Linux).
SYSTEM_PACKAGES=(libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev
  libwayland-dev libxkbcommon-dev libfreetype-dev libsystemd-dev libcurl4-openssl-dev libpolkit-agent-1-dev
  libbpf-dev dbus-daemon pkg-config)
# The apt.llvm.org packages this job uses by name: llvm.sh's own four, then the extras. Their
# dependencies (libllvm, libclang-cpp, ...) come along.
LLVM_PACKAGES=("clang-${LLVM_VER}" "lldb-${LLVM_VER}" "lld-${LLVM_VER}" "clangd-${LLVM_VER}"
  "libc++-${LLVM_VER}-dev" "libc++abi-${LLVM_VER}-dev")
if [[ "${INSTALL_NINJA:-}" == "true" ]]; then
  SYSTEM_PACKAGES+=(ninja-build)
fi
if [[ "${INSTALL_CCACHE:-}" == "true" ]]; then
  SYSTEM_PACKAGES+=(ccache)
fi
if [[ "${INSTALL_CLANG_FORMAT:-}" == "true" ]]; then
  LLVM_PACKAGES+=("clang-format-${LLVM_VER}")
fi
if [[ "${INSTALL_CLANG_TIDY:-}" == "true" ]]; then
  LLVM_PACKAGES+=("clang-tidy-${LLVM_VER}")
fi
# clangd is one of llvm.sh's four, so INSTALL_CLANGD needs nothing more here.

apt_update() {
  sudo timeout --kill-after=10s 180s apt-get update
}
apt_install() {
  sudo timeout --kill-after=10s 300s apt-get install -y "$@"
}

# --- Cache hit: install the restored .debs, with no apt.llvm.org source configured. --------------
if [[ -n "$EXPECT_RELEASE" && -n "$CACHE_DIR" && "$CACHE_RESTORED" == "true" ]]; then
  mapfile -t debs < <(find "$CACHE_DIR" -maxdepth 1 -type f -name '*.deb' | LC_ALL=C sort)
  complete=true
  for pkg in "${LLVM_PACKAGES[@]}"; do
    if ! compgen -G "$CACHE_DIR/${pkg}_*.deb" >/dev/null; then
      echo "::warning::The LLVM package cache has no $pkg; installing from apt.llvm.org instead."
      complete=false
    fi
  done
  if [[ "$complete" == "true" ]]; then
    echo "Installing LLVM ${EXPECT_RELEASE} from the package cache (${#debs[@]} .debs); apt.llvm.org is not contacted."
    retry apt_update
    retry apt_install "${debs[@]}" "${SYSTEM_PACKAGES[@]}"
    echo "cache-hit=true" >> "$OUTPUT"
    echo "staged=false" >> "$OUTPUT"
    exit 0
  fi
fi
echo "cache-hit=false" >> "$OUTPUT"

# --- Miss: apt.llvm.org. ---------------------------------------------------------------------------
# The repository key, fetched here with retries so llvm.sh (which only downloads it when the file is
# missing, with 3 quick tries) finds it in place.
if [[ ! -f /etc/apt/trusted.gpg.d/apt.llvm.org.asc ]]; then
  key_file=$(mktemp)
  curl --fail --silent --show-error --location --connect-timeout 20 \
    --retry 5 --retry-delay 10 --retry-all-errors \
    --output "$key_file" https://apt.llvm.org/llvm-snapshot.gpg.key
  sudo install -m 0644 "$key_file" /etc/apt/trusted.gpg.d/apt.llvm.org.asc
  rm -f "$key_file"
fi

# Verify the installer before granting it root privileges.
# To update: curl --fail --silent --show-error --location --output llvm.sh https://apt.llvm.org/llvm.sh && sha256sum llvm.sh | awk '{print $1}'
LLVM_INSTALLER_SHA256='03878e08f47b66cc95bc4b544b0db3c6d9ce8d60e6cf2492ae357984330a9eae'
curl --fail --silent --show-error --location --connect-timeout 20 \
  --retry 5 --retry-delay 10 --retry-all-errors \
  --output llvm.sh https://apt.llvm.org/llvm.sh
echo "$LLVM_INSTALLER_SHA256  llvm.sh" | sha256sum --check --strict
chmod +x llvm.sh
# llvm.sh is safe to re-run: add-apt-repository skips a source that's already there.
retry sudo timeout --kill-after=10s 300s ./llvm.sh "$LLVM_VER"
rm -f llvm.sh

retry apt_update
retry apt_install "${LLVM_PACKAGES[@]}" "${SYSTEM_PACKAGES[@]}"

# --- Stage the release's .debs for the caller to save. ---------------------------------------------
# Every installed package whose version is this release's apt.llvm.org build (1:<release>~...): the
# named packages and the LLVM libraries they pulled in, all built from the one llvm-toolchain source.
# apt-get keeps what it downloaded in /var/cache/apt/archives; anything missing is downloaded again.
# Staging is an optimisation, so a failure only leaves staged=false.
stage() {
  local -a pins missing=()
  local pin pkg ver file
  mapfile -t pins < <(dpkg-query -W -f='${db:Status-Abbrev}${Package}=${Version}\n' |
    awk -v prefix="=1:${EXPECT_RELEASE}~" '/^ii/ && index($0, prefix) { sub(/^ii +/, ""); print }')
  if (( ${#pins[@]} == 0 )); then
    echo "::warning::No installed package is LLVM ${EXPECT_RELEASE}'s; nothing to cache."
    return 1
  fi
  # Called as `if stage`, so set -e is off in here: every step that can fail returns by itself.
  rm -rf "$CACHE_DIR" || return 1
  mkdir -p "$CACHE_DIR" || return 1
  for pin in "${pins[@]}"; do
    pkg=${pin%%=*}
    ver=${pin#*=}
    # Archive file names spell the epoch's ':' as %3a.
    file=$(compgen -G "/var/cache/apt/archives/${pkg}_${ver//:/%3a}_*.deb" | head -n 1 || true)
    if [[ -n "$file" ]]; then
      cp "$file" "$CACHE_DIR/" || return 1
    else
      missing+=("$pin")
    fi
  done
  if (( ${#missing[@]} > 0 )); then
    echo "Downloading ${#missing[@]} LLVM .debs apt-get didn't keep."
    (cd "$CACHE_DIR" && retry apt-get download "${missing[@]}") || return 1
  fi
  # apt-get download names files by package, so each pin must now have exactly one.
  for pin in "${pins[@]}"; do
    pkg=${pin%%=*}
    if ! compgen -G "$CACHE_DIR/${pkg}_*.deb" >/dev/null; then
      echo "::warning::$pkg is missing from the staged LLVM packages."
      return 1
    fi
  done
  echo "Staged ${#pins[@]} LLVM ${EXPECT_RELEASE} .debs ($(du -sh "$CACHE_DIR" | cut -f1)) in $CACHE_DIR."
}
if [[ -n "$EXPECT_RELEASE" && -n "$CACHE_DIR" ]]; then
  if stage; then
    echo "staged=true" >> "$OUTPUT"
  else
    echo "::warning::Could not stage the LLVM packages; this run saves no package cache."
    echo "staged=false" >> "$OUTPUT"
  fi
else
  echo "staged=false" >> "$OUTPUT"
fi
