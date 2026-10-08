#!/usr/bin/env bash
# .devcontainer/install-toolchain.sh -- install the TaskSmack Linux toolchain into the dev
# container image. Runs once, as root, from .devcontainer/Dockerfile at image build time.
#
# Usage: install-toolchain.sh <llvm-major> <python-major.minor> <user>
#
# The package lists mirror what Linux CI installs, so a build inside the container matches CI:
#   - LLVM toolchain + GUI build deps: .github/actions/setup-llvm/action.yml
#   - Xvfb + Mesa software GL for the display-dependent tests: .github/workflows/reusable-build-test.yml
#   - CMake (Kitware apt), Python (deadsnakes), lld/llvm, and the repository signing-key
#     fingerprints: tools/setup-dev.sh (CI runners already ship a CMake >= 3.29 and use
#     actions/setup-python; Ubuntu 24.04's own cmake 3.28 and python 3.12 are too old).

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $(basename "$0") <llvm-major> <python-major.minor> <user>" >&2
    exit 2
fi

LLVM_VERSION="$1"
PYTHON_VERSION="$2"
DEV_USER="$3"

if [[ ! "$LLVM_VERSION" =~ ^[0-9]+$ ]]; then
    echo "Error: LLVM version must be a numeric major version, got '$LLVM_VERSION'." >&2
    exit 2
fi
if [[ ! "$PYTHON_VERSION" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Python version must be major.minor, got '$PYTHON_VERSION'." >&2
    exit 2
fi

export DEBIAN_FRONTEND=noninteractive

apt_install() {
    apt-get install -y --no-install-recommends "$@"
}

# ── Base tools (things the GitHub ubuntu-24.04 runner image already has) ─────────────────────
apt-get update
apt_install \
    ca-certificates \
    curl \
    git \
    gnupg \
    less \
    lsb-release \
    openssh-client \
    pkg-config \
    software-properties-common \
    sudo \
    wget

# ── Signed package repositories: Kitware (CMake), deadsnakes (Python), apt.llvm.org (LLVM) ───
# Same fingerprints as tools/setup-dev.sh; update both together.
KITWARE_KEY_FINGERPRINT="4DBEBE3EEC96E7B8C6EC5BE99E92FDC6C5B9BA75"
LLVM_KEY_FINGERPRINT="6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"

# Refuse a key file unless it holds exactly one primary key with the expected fingerprint
# (same check as tools/setup-dev.sh: subkey fingerprints are ignored, a second primary key fails).
verify_key_fingerprint() {
    local label="$1" keyfile="$2" expected="$3"
    local -a primary_fprs=()
    local want_fpr=0 record_type fpr
    while IFS=: read -r record_type _ _ _ _ _ _ _ _ fpr _; do
        case "$record_type" in
            pub) want_fpr=1 ;;
            fpr)
                if [[ "$want_fpr" == 1 ]]; then
                    primary_fprs+=("$fpr")
                fi
                want_fpr=0
                ;;
            *) want_fpr=0 ;;
        esac
    done < <(gpg --show-keys --with-fingerprint --with-colons "$keyfile" 2>/dev/null)

    if [[ ${#primary_fprs[@]} -ne 1 || "${primary_fprs[0]}" != "$expected" ]]; then
        echo "Error: $label signing key fingerprint mismatch (expected $expected)." >&2
        printf '  found: %s\n' "${primary_fprs[@]}" >&2
        exit 1
    fi
    echo "Verified $label signing key fingerprint: ${primary_fprs[0]}"
}

# shellcheck disable=SC1091
source /etc/os-release
CODENAME="${UBUNTU_CODENAME:?not an Ubuntu image}"

KEY_DIR="$(mktemp -d)"
trap 'rm -rf "$KEY_DIR"' EXIT

curl --fail --silent --show-error --location \
    --output "$KEY_DIR/kitware.asc" https://apt.kitware.com/keys/kitware-archive-latest.asc
verify_key_fingerprint "Kitware" "$KEY_DIR/kitware.asc" "$KITWARE_KEY_FINGERPRINT"
gpg --dearmor <"$KEY_DIR/kitware.asc" >/usr/share/keyrings/kitware-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${CODENAME} main" \
    >/etc/apt/sources.list.d/kitware.list

curl --fail --silent --show-error --location \
    --output "$KEY_DIR/llvm.asc" https://apt.llvm.org/llvm-snapshot.gpg.key
verify_key_fingerprint "LLVM" "$KEY_DIR/llvm.asc" "$LLVM_KEY_FINGERPRINT"
gpg --dearmor <"$KEY_DIR/llvm.asc" >/usr/share/keyrings/llvm-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" \
    >/etc/apt/sources.list.d/llvm.list

add-apt-repository -y ppa:deadsnakes/ppa
apt-get update

# ── Toolchain and build dependencies ─────────────────────────────────────────────────────────
apt_install \
    cmake \
    ninja-build \
    ccache \
    "python${PYTHON_VERSION}" \
    "python${PYTHON_VERSION}-venv" \
    "clang-${LLVM_VERSION}" \
    "clang-format-${LLVM_VERSION}" \
    "clang-tidy-${LLVM_VERSION}" \
    "clangd-${LLVM_VERSION}" \
    "lld-${LLVM_VERSION}" \
    "lldb-${LLVM_VERSION}" \
    "llvm-${LLVM_VERSION}" \
    "libc++-${LLVM_VERSION}-dev" \
    "libc++abi-${LLVM_VERSION}-dev" \
    libfreetype6-dev \
    libgl1-mesa-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libxext-dev \
    libwayland-dev \
    libxkbcommon-dev

# Headless display for the GL/window test suites (CI runs ctest under xvfb-run).
apt_install xvfb xauth libgl1-mesa-dri libglx-mesa0 libegl1 libegl-mesa0

# Unversioned tool names, as .github/actions/setup-llvm/action.yml configures them.
for tool in clang clang++ lld ld.lld clang-format clang-tidy clangd llvm-profdata llvm-cov; do
    update-alternatives --install "/usr/bin/${tool}" "${tool}" "/usr/bin/${tool}-${LLVM_VERSION}" 100
    update-alternatives --set "${tool}" "/usr/bin/${tool}-${LLVM_VERSION}"
done

apt-get clean
rm -rf /var/lib/apt/lists/*

# ── Non-root development user ────────────────────────────────────────────────────────────────
if ! id "$DEV_USER" &>/dev/null; then
    useradd --create-home --shell /bin/bash "$DEV_USER"
fi
echo "${DEV_USER} ALL=(root) NOPASSWD:ALL" >"/etc/sudoers.d/${DEV_USER}"
chmod 0440 "/etc/sudoers.d/${DEV_USER}"

echo "=== Toolchain ==="
clang++ --version | head -1
ld.lld --version
clang-tidy --version | head -1
clang-format --version
cmake --version | head -1
echo "ninja $(ninja --version)"
ccache --version | head -1
"python${PYTHON_VERSION}" --version
