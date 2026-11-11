#!/usr/bin/env bash
# Install LLVM <major> and the build's system packages on an Ubuntu runner (setup-llvm, #3393, #3635).
#
# Environment (from action.yml): LLVM_VERSION (major), EXPECT_RELEASE (e.g. 23.1.5, or empty),
# CACHE_DIR / CACHE_RESTORED / CACHE_MATCHED_KEY (the LLVM package cache: its directory, whether the
# exact key was restored, and the key actions/cache/restore matched, which is an older entry's when
# only a restore-key matched), INSTALL_* (the action's optional tools). Writes cache-hit and staged
# to $GITHUB_OUTPUT.
#
# Three paths:
#   - Cache hit (the exact key): every LLVM .deb this job needs was restored into CACHE_DIR, so they
#     are installed from there and apt.llvm.org is never contacted. apt-get still pulls their Ubuntu
#     dependencies and the system packages from the Ubuntu archive.
#   - Miss: apt.llvm.org's llvm.sh adds the repository and installs clang, then apt-get installs the
#     rest. Every network step retries with backoff, since apt.llvm.org's key, installer and
#     repository have each failed on GitHub's runners for hours (2026-11-03: "Could not resolve host:
#     apt.llvm.org"; 2026-11-11: timeouts, #3635). The installed LLVM .debs are then staged in
#     CACHE_DIR, to be saved under the exact key.
#   - An older entry (no expect-release only): a restore-key matched an earlier week's entry, so the
#     run refreshes from apt.llvm.org as on a miss, with two tries instead of four. When apt.llvm.org
#     still fails, it installs the older entry's .debs with a warning instead of failing the job.
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
CACHE_MATCHED_KEY="${CACHE_MATCHED_KEY:-}"
OUTPUT="${GITHUB_OUTPUT:-/dev/null}"

# Runs "$@" up to RETRY_ATTEMPTS (4) times, waiting 15, 30 and 60 seconds between tries (about 2
# minutes in all, plus each try's own timeout). Bounded, so a real outage still fails the job instead
# of hanging it. RETRY_GIVE_UP is the annotation for the last failure: a warning when an older cache
# entry stands behind it, so a job that passes on that entry shows no error.
RETRY_ATTEMPTS=4
RETRY_GIVE_UP=error
retry() {
  local attempt=1 delay=15 status
  while true; do
    if "$@"; then
      return 0
    else
      status=$?
    fi
    if (( attempt >= RETRY_ATTEMPTS )); then
      echo "::${RETRY_GIVE_UP}::'$*' failed $attempt times (last exit $status)."
      return "$status"
    fi
    echo "::warning::'$*' failed (exit $status), try $attempt of $RETRY_ATTEMPTS; retrying in ${delay}s."
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
# the UDP byte counter TaskSmack loads as root (#3366, #3580; libbpf required on Linux).
SYSTEM_PACKAGES=(libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev
  libwayland-dev libxkbcommon-dev libfreetype-dev libsystemd-dev libcurl4-openssl-dev
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

if [[ -n "$EXPECT_RELEASE" ]]; then
  LLVM_LABEL="LLVM ${EXPECT_RELEASE}"
else
  LLVM_LABEL="LLVM ${LLVM_VER}"
fi

apt_update() {
  sudo timeout --kill-after=10s 180s apt-get update
}
apt_install() {
  sudo timeout --kill-after=10s 300s apt-get install -y "$@"
}

# --- What the package cache restored. ---------------------------------------------------------------
# exact: this run's own key. older: a restore-key's match, the newest earlier entry of the same
# package set (no expect-release only: a release restores its exact key or nothing). none: nothing,
# or a set missing one of this job's packages.
restored=none
if [[ -n "$CACHE_DIR" ]]; then
  if [[ "$CACHE_RESTORED" == "true" ]]; then
    restored=exact
  elif [[ -z "$EXPECT_RELEASE" && -n "$CACHE_MATCHED_KEY" ]]; then
    restored=older
  fi
fi
cached_debs=()
if [[ "$restored" != "none" ]]; then
  mapfile -t cached_debs < <(find "$CACHE_DIR" -maxdepth 1 -type f -name '*.deb' | LC_ALL=C sort)
  for pkg in "${LLVM_PACKAGES[@]}"; do
    if ! compgen -G "$CACHE_DIR/${pkg}_*.deb" >/dev/null; then
      echo "::warning::The LLVM package cache has no $pkg; installing from apt.llvm.org instead."
      restored=none
    fi
  done
fi

# Installs the restored .debs and the system packages with no apt.llvm.org source configured. A
# source a failed llvm.sh left behind is removed first, or apt-get update would fail on it while
# apt.llvm.org is down; --allow-downgrades lets an older entry replace whatever a partial
# apt.llvm.org install put in.
install_cached() {
  local -a llvm_sources=()
  mapfile -t llvm_sources < <(grep -rlE 'apt\.llvm\.org' /etc/apt/sources.list.d 2>/dev/null || true)
  if (( ${#llvm_sources[@]} > 0 )); then
    sudo rm -f "${llvm_sources[@]}"
  fi
  if [[ -f /etc/apt/sources.list ]] && grep -qE 'apt\.llvm\.org' /etc/apt/sources.list; then
    sudo sed -i '/apt\.llvm\.org/d' /etc/apt/sources.list
  fi
  retry apt_update
  retry apt_install --allow-downgrades "${cached_debs[@]}" "${SYSTEM_PACKAGES[@]}"
}

# --- Cache hit: install the restored .debs. ---------------------------------------------------------
if [[ "$restored" == "exact" ]]; then
  echo "Installing ${LLVM_LABEL} from the package cache (${#cached_debs[@]} .debs); apt.llvm.org is not contacted."
  install_cached
  echo "cache-hit=true" >> "$OUTPUT"
  echo "staged=false" >> "$OUTPUT"
  exit 0
fi
echo "cache-hit=false" >> "$OUTPUT"

# --- Miss: apt.llvm.org. ----------------------------------------------------------------------------
# Called as `if install_from_apt_llvm`, so set -e is off in here: every step that can fail returns by
# itself. Returns 3 when the installer's checksum doesn't match, which an older cache entry must not
# paper over: that's a changed installer to review, not an outage.
install_from_apt_llvm() {
  # The repository key, fetched here with retries so llvm.sh (which only downloads it when the file
  # is missing, with 3 quick tries) finds it in place.
  if [[ ! -f /etc/apt/trusted.gpg.d/apt.llvm.org.asc ]]; then
    local key_file
    key_file=$(mktemp) || return 1
    curl --fail --silent --show-error --location --connect-timeout 20 --max-time 120 \
      --retry 5 --retry-delay 10 --retry-all-errors \
      --output "$key_file" https://apt.llvm.org/llvm-snapshot.gpg.key || return 1
    sudo install -m 0644 "$key_file" /etc/apt/trusted.gpg.d/apt.llvm.org.asc || return 1
    rm -f "$key_file"
  fi

  # Verify the installer before granting it root privileges.
  # To update: curl --fail --silent --show-error --location --output llvm.sh https://apt.llvm.org/llvm.sh && sha256sum llvm.sh | awk '{print $1}'
  local LLVM_INSTALLER_SHA256='03878e08f47b66cc95bc4b544b0db3c6d9ce8d60e6cf2492ae357984330a9eae'
  curl --fail --silent --show-error --location --connect-timeout 20 --max-time 120 \
    --retry 5 --retry-delay 10 --retry-all-errors \
    --output llvm.sh https://apt.llvm.org/llvm.sh || return 1
  echo "$LLVM_INSTALLER_SHA256  llvm.sh" | sha256sum --check --strict || return 3
  chmod +x llvm.sh || return 1
  # llvm.sh is safe to re-run: add-apt-repository skips a source that's already there.
  retry sudo timeout --kill-after=10s 300s ./llvm.sh "$LLVM_VER" || return 1
  rm -f llvm.sh

  retry apt_update || return 1
  retry apt_install "${LLVM_PACKAGES[@]}" "${SYSTEM_PACKAGES[@]}" || return 1
}

if [[ "$restored" == "older" ]]; then
  # The older entry covers an outage, so two tries are enough to tell one, not 20 minutes of four.
  echo "The package cache has an older ${LLVM_LABEL} entry (${CACHE_MATCHED_KEY}); refreshing it from apt.llvm.org."
  RETRY_ATTEMPTS=2
  RETRY_GIVE_UP=warning
fi
if install_from_apt_llvm; then
  status=0
else
  status=$?
fi
RETRY_ATTEMPTS=4
RETRY_GIVE_UP=error
if (( status != 0 )); then
  if [[ "$restored" == "older" && "$status" -ne 3 ]]; then
    echo "::warning::apt.llvm.org failed (exit $status); installing ${LLVM_LABEL} from the older package cache entry ${CACHE_MATCHED_KEY} (${#cached_debs[@]} .debs) instead. A later run refreshes it."
    rm -f llvm.sh
    install_cached
    # Not saved again under this run's key, so the next run still tries apt.llvm.org for a fresh one.
    echo "staged=false" >> "$OUTPUT"
    exit 0
  fi
  exit "$status"
fi

# --- Stage the installed LLVM .debs for saving. -----------------------------------------------------
# Every installed package of clang-<major>'s own apt.llvm.org build (with EXPECT_RELEASE, of that
# release's, 1:<release>~...): the named packages and the LLVM libraries they pulled in, all built
# from the one llvm-toolchain source. apt-get keeps what it downloaded in /var/cache/apt/archives;
# anything missing is downloaded again. Staging is an optimisation, so a failure only leaves
# staged=false.
stage() {
  local -a pins missing=()
  local pin pkg ver file want exact
  # Called as `if stage`, so set -e is off in here: every step that can fail returns by itself.
  if [[ -n "$EXPECT_RELEASE" ]]; then
    want="1:${EXPECT_RELEASE}~"
    exact=0
  else
    want=$(dpkg-query -W -f='${Version}' "clang-${LLVM_VER}") || return 1
    exact=1
  fi
  mapfile -t pins < <(dpkg-query -W -f='${db:Status-Abbrev}${Package}=${Version}\n' |
    awk -v want="$want" -v exact="$exact" '/^ii/ {
      sub(/^ii +/, ""); split($0, f, "=")
      if (exact ? f[2] == want : index(f[2], want) == 1) print
    }')
  if (( ${#pins[@]} == 0 )); then
    echo "::warning::No installed package is ${LLVM_LABEL}'s; nothing to cache."
    return 1
  fi
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
  echo "Staged ${#pins[@]} ${LLVM_LABEL} .debs ($(du -sh "$CACHE_DIR" | cut -f1)) in $CACHE_DIR."
}
if [[ -n "$CACHE_DIR" ]]; then
  if stage; then
    echo "staged=true" >> "$OUTPUT"
  else
    echo "::warning::Could not stage the LLVM packages; this run saves no package cache."
    echo "staged=false" >> "$OUTPUT"
  fi
else
  echo "staged=false" >> "$OUTPUT"
fi
