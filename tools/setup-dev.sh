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
LLVM_VERSION=22

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Install TaskSmack development prerequisites on Ubuntu.

Options:
  --dry-run    Print apt commands without executing them
  --minimal    Install build prerequisites only; skip coverage/profiling/format tools
  --llvm VER   LLVM major version to install (currently must be 22)
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

if [[ "$LLVM_VERSION" != "22" ]]; then
    echo "Error: TaskSmack presets currently require LLVM 22; received LLVM $LLVM_VERSION." >&2
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

    wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc |
        gpg --dearmor |
        sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${UBUNTU_CODENAME} main" |
        sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null

    sudo add-apt-repository -y ppa:deadsnakes/ppa

    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key |
        gpg --dearmor |
        sudo tee /usr/share/keyrings/llvm-archive-keyring.gpg >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/${UBUNTU_CODENAME}/ llvm-toolchain-${UBUNTU_CODENAME}-${LLVM_VERSION} main" |
        sudo tee /etc/apt/sources.list.d/llvm.list >/dev/null

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
