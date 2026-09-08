#!/usr/bin/env bash
# tools/setup-dev.sh — Install TaskSmack development prerequisites on Linux.
#
# Usage:
#   ./tools/setup-dev.sh              # Install all prerequisites
#   ./tools/setup-dev.sh --dry-run    # Print what would be installed without running apt
#   ./tools/setup-dev.sh --minimal    # Install build prerequisites only (skip coverage/profiling tools)
#
# After running this script, verify your environment with: ./tools/check-prereqs.sh
#
# See CONTRIBUTING.md for full documentation of prerequisites.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

DRY_RUN=false
MINIMAL=false
# Single source of truth for the LLVM major version this repo's presets/CI currently
# validate against -- Renovate's LLVM customManager (.github/renovate.json5) bumps this
# literal, and the --llvm guard below reads it back rather than hardcoding "22" a second
# time, so a Renovate-proposed bump can't silently desync the guard from the default it's
# supposed to be checking.
readonly LLVM_SUPPORTED_VERSION=22
LLVM_VERSION=$LLVM_SUPPORTED_VERSION

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Install TaskSmack development prerequisites on Ubuntu.

Options:
  --dry-run    Print apt commands without executing them
  --minimal    Install build prerequisites only; skip coverage/profiling/format tools
  --llvm VER   LLVM major version to install (currently must be $LLVM_SUPPORTED_VERSION)
  -h, --help   Show this help
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --minimal) MINIMAL=true; shift ;;
        --llvm)
            if [[ $# -lt 2 ]]; then
                echo "Error: --llvm requires a version." >&2
                exit 2
            fi
            LLVM_VERSION="$2"
            shift 2
            ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if [[ "$LLVM_VERSION" != "$LLVM_SUPPORTED_VERSION" ]]; then
    echo "Error: TaskSmack presets currently require LLVM $LLVM_SUPPORTED_VERSION; received LLVM $LLVM_VERSION." >&2
    exit 2
fi

run_apt() {
    if $DRY_RUN; then
        echo "[dry-run] sudo apt-get install -y $*"
    else
        sudo apt-get install -y "$@"
    fi
}

run_cmd() {
    if $DRY_RUN; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

echo "=== TaskSmack Dev Setup ==="
echo "LLVM version: $LLVM_VERSION"
if $DRY_RUN; then echo "(dry-run mode — nothing will be installed)"; fi
echo ""

# ── Step 1: Signed package repositories ───────────────────────────────────────
echo "==> Configuring signed package repositories..."
if $DRY_RUN; then
    echo "[dry-run] sudo apt-get update"
else
    sudo apt-get update
fi
run_apt ca-certificates gnupg software-properties-common wget

if $DRY_RUN; then
    echo "[dry-run] add signed Kitware, deadsnakes, and apt.llvm.org repositories"
else
    # shellcheck disable=SC1091
    source /etc/os-release
    if [[ "${ID:-}" != "ubuntu" || -z "${UBUNTU_CODENAME:-}" ]]; then
        echo "Error: automatic setup currently supports Ubuntu only." >&2
        exit 1
    fi

    # Expected fingerprints, cross-checked against each vendor's own published
    # setup instructions (https://apt.kitware.com/, https://apt.llvm.org/) at
    # the time this check was added. If a vendor rotates their signing key,
    # this script will fail loudly with the new key's actual fingerprint —
    # update the expected value below only after independently confirming the
    # new fingerprint via the vendor's official documentation, not just by
    # trusting the download.
    KITWARE_KEY_FINGERPRINT="4DBEBE3EEC96E7B8C6EC5BE99E92FDC6C5B9BA75"
    LLVM_KEY_FINGERPRINT="6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"

    verify_key_fingerprint() {
        local label="$1" keyfile="$2" expected="$3"
        # Only count fingerprints that follow a "pub:" record (the primary key), not a
        # "sub:" record (a subkey) -- a real key legitimately has subkey fingerprints that
        # differ from its own. Collecting a plain first-match would let a tampered file place
        # the real key first and append an attacker-controlled *second primary key*, which gpg
        # --dearmor would still install and apt would then also trust. Requiring exactly one
        # primary key total closes that regardless of ordering.
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

        if [[ ${#primary_fprs[@]} -ne 1 ]]; then
            echo "Error: $label key file contains ${#primary_fprs[@]} primary key(s); expected exactly 1." >&2
            printf '  found: %s\n' "${primary_fprs[@]}" >&2
            echo "  Refusing to trust a key file with an unexpected number of primary keys." >&2
            exit 1
        fi

        local actual="${primary_fprs[0]}"
        if [[ "$actual" != "$expected" ]]; then
            echo "Error: $label signing key fingerprint mismatch." >&2
            echo "  expected: $expected" >&2
            echo "  actual:   ${actual:-<none>}" >&2
            echo "  Do not install this key. If $label rotated its signing key," >&2
            echo "  confirm the new fingerprint via their official documentation" >&2
            echo "  and update KITWARE_KEY_FINGERPRINT/LLVM_KEY_FINGERPRINT in" >&2
            echo "  tools/setup-dev.sh (and tools/setup-dev.ps1 if applicable)." >&2
            exit 1
        fi
        echo "==> Verified $label signing key fingerprint: $actual"
    }

    KITWARE_KEY_FILE=$(mktemp)
    LLVM_KEY_FILE=$(mktemp)
    trap 'rm -f "$KITWARE_KEY_FILE" "$LLVM_KEY_FILE"' EXIT

    wget -qO "$KITWARE_KEY_FILE" https://apt.kitware.com/keys/kitware-archive-latest.asc
    verify_key_fingerprint "Kitware" "$KITWARE_KEY_FILE" "$KITWARE_KEY_FINGERPRINT"
    gpg --dearmor < "$KITWARE_KEY_FILE" |
        sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${UBUNTU_CODENAME} main" |
        sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null

    sudo add-apt-repository -y ppa:deadsnakes/ppa

    wget -qO "$LLVM_KEY_FILE" https://apt.llvm.org/llvm-snapshot.gpg.key
    verify_key_fingerprint "LLVM" "$LLVM_KEY_FILE" "$LLVM_KEY_FINGERPRINT"
    gpg --dearmor < "$LLVM_KEY_FILE" |
        sudo tee /usr/share/keyrings/llvm-archive-keyring.gpg >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/${UBUNTU_CODENAME}/ llvm-toolchain-${UBUNTU_CODENAME}-${LLVM_VERSION} main" |
        sudo tee /etc/apt/sources.list.d/llvm.list >/dev/null

    rm -f "$KITWARE_KEY_FILE" "$LLVM_KEY_FILE"
    trap - EXIT

    sudo apt-get update
fi

# ── Step 2: Base build tools ──────────────────────────────────────────────────
echo "==> Installing base build tools (CMake, Ninja, Python 3.14, ccache)..."
run_apt cmake ninja-build python3.14 python3.14-venv ccache libfreetype6-dev

PYTHON_ENV="${REPO_ROOT}/.venv"
run_cmd python3.14 -m venv "$PYTHON_ENV"
run_cmd "$PYTHON_ENV/bin/python" -m pip install --upgrade pip
run_cmd "$PYTHON_ENV/bin/python" -m pip install --require-hashes -r "${REPO_ROOT}/requirements-glad.lock"

# ── Step 3: LLVM / Clang toolchain ───────────────────────────────────────────
echo ""
echo "==> Installing LLVM $LLVM_VERSION toolchain..."
run_apt \
    "clang-$LLVM_VERSION" \
    "clang-tidy-$LLVM_VERSION" \
    "clang-format-$LLVM_VERSION" \
    "lld-$LLVM_VERSION" \
    "llvm-$LLVM_VERSION" \
    "libc++-$LLVM_VERSION-dev" \
    "libc++abi-$LLVM_VERSION-dev"

# Register update-alternatives so unversioned names (clang++, clang-tidy, etc.) point to the
# installed LLVM version.  Idempotent: re-running updates to a higher priority.
if ! $DRY_RUN; then
    sudo update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-$LLVM_VERSION" 100
    sudo update-alternatives --install /usr/bin/clang   clang   "/usr/bin/clang-$LLVM_VERSION"   100
    sudo update-alternatives --install /usr/bin/lld     lld     "/usr/bin/lld-$LLVM_VERSION"     100
else
    echo "[dry-run] sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-$LLVM_VERSION 100"
    echo "[dry-run] sudo update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-$LLVM_VERSION   100"
    echo "[dry-run] sudo update-alternatives --install /usr/bin/lld     lld     /usr/bin/lld-$LLVM_VERSION     100"
fi

# ── Step 4: Build-time GUI prerequisites ─────────────────────────────────────
echo ""
echo "==> Installing GUI build prerequisites..."
run_apt \
    libgl1-mesa-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libxext-dev \
    libwayland-dev \
    libxkbcommon-dev

if ! $MINIMAL; then
    # ── Step 5: Coverage and profiling tools ─────────────────────────────────
    echo ""
    echo "==> Installing coverage and profiling tools..."
    run_apt "clangd-$LLVM_VERSION"
    if ! $DRY_RUN; then
        sudo update-alternatives --install /usr/bin/llvm-profdata llvm-profdata "/usr/bin/llvm-profdata-$LLVM_VERSION" 100
        sudo update-alternatives --install /usr/bin/llvm-cov      llvm-cov      "/usr/bin/llvm-cov-$LLVM_VERSION"      100
    else
        echo "[dry-run] sudo update-alternatives --install /usr/bin/llvm-profdata ..."
        echo "[dry-run] sudo update-alternatives --install /usr/bin/llvm-cov ..."
    fi

    # heaptrack (tools/profile-heap.sh) and the FlameGraph scripts (analyze-perf.sh) are
    # zero-risk to install here: heaptrack is a plain apt package, FlameGraph is just a
    # git clone. Neither is coupled to the running kernel version, unlike `perf` itself
    # (linux-tools-$(uname -r)) -- that one is deliberately left as a manual step;
    # check-prereqs.sh already detects and prints the exact install command for it, and
    # getting the wrong kernel-flavor package here could fail in ways that are confusing
    # to unwind, especially on WSL2 (see the WSL2 notes in this script and
    # tools/profile-perf.sh).
    run_apt heaptrack
    if [[ ! -d "$HOME/opt/FlameGraph" ]]; then
        if ! command -v git &>/dev/null; then
            echo "WARNING: git not found; skipping FlameGraph clone. Install git and re-run, or clone" >&2
            echo "         manually: git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph" >&2
        else
            # git clone fails if the destination's parent directory doesn't exist yet.
            run_cmd mkdir -p "$HOME/opt"
            # Best-effort: FlameGraph is optional tooling, so a transient network/proxy
            # failure here shouldn't abort the rest of setup under `set -e`.
            if ! run_cmd git clone --depth 1 https://github.com/brendangregg/FlameGraph "$HOME/opt/FlameGraph"; then
                echo "WARNING: FlameGraph clone failed; continuing setup. Retry manually with:" >&2
                echo "         git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph" >&2
            fi
        fi
    fi

    # ── Step 6: Headless test runtime ────────────────────────────────────────
    echo ""
    echo "==> Installing headless test runtime..."
    run_apt xvfb

    # ── Step 7: Development Python dependencies ─────────────────────────────
    echo ""
    echo "==> Installing development Python dependencies..."
    run_cmd "$PYTHON_ENV/bin/python" -m pip install -r "${REPO_ROOT}/requirements.txt"
fi

echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. Activate Python:     source .venv/bin/activate"
echo "  2. Verify environment:  ./tools/check-prereqs.sh"
echo "  3. Configure project:   cmake --preset debug"
echo "  4. Build:               cmake --build --preset debug"
echo "  5. Run tests:           ctest --preset debug"
echo ""
echo "For full documentation see CONTRIBUTING.md"
