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

# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

DRY_RUN=false
MINIMAL=false
LLVM_VERSION=22

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Install TaskSmack development prerequisites on Linux (Ubuntu/Debian).

Options:
  --dry-run    Print apt commands without executing them
  --minimal    Install build prerequisites only; skip coverage/profiling/format tools
  --llvm VER   LLVM major version to install (default: $LLVM_VERSION)
  -h, --help   Show this help
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --minimal) MINIMAL=true; shift ;;
        --llvm) LLVM_VERSION="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

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

# ── Step 1: Base build tools ──────────────────────────────────────────────────
echo "==> Installing base build tools (cmake, ninja, python3, ccache)..."
if $DRY_RUN; then
    echo "[dry-run] sudo apt-get update"
else
    sudo apt-get update
fi
run_apt cmake ninja-build python3 python3-pip python3-jinja2 ccache libfreetype6-dev

# ── Step 2: LLVM / Clang toolchain ───────────────────────────────────────────
echo ""
echo "==> Installing LLVM $LLVM_VERSION toolchain..."
if ! command -v wget &>/dev/null; then
    run_apt wget
fi

if $DRY_RUN; then
    echo "[dry-run] download and run llvm.sh for LLVM $LLVM_VERSION"
else
    wget -q https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh
    chmod +x /tmp/llvm.sh
    sudo /tmp/llvm.sh "$LLVM_VERSION"
    rm -f /tmp/llvm.sh
fi

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

if ! $MINIMAL; then
    # ── Step 3: Coverage and profiling tools ─────────────────────────────────
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

    # ── Step 4: GUI/display prerequisites (headless test runs) ───────────────
    echo ""
    echo "==> Installing GUI prerequisites (xvfb, X11 libs)..."
    run_apt \
        xvfb \
        libgl1-mesa-dev \
        libx11-dev \
        libxrandr-dev \
        libxinerama-dev \
        libxcursor-dev \
        libxi-dev \
        libxext-dev \
        libwayland-dev \
        libxkbcommon-dev

    # ── Step 5: Optional tools (pre-commit) ──────────────────────────────────
    echo ""
    echo "==> Installing pre-commit (optional but recommended)..."
    if $DRY_RUN; then
        echo "[dry-run] pip3 install --user pre-commit"
    else
        pip3 install --user pre-commit 2>/dev/null || pip install --user pre-commit 2>/dev/null || \
            echo "  Note: pip install of pre-commit failed; install manually: pip3 install pre-commit"
    fi
fi

echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. Verify environment:  ./tools/check-prereqs.sh"
echo "  2. Configure project:   cmake --preset debug"
echo "  3. Build:               cmake --build --preset debug"
echo "  4. Run tests:           ctest --preset debug"
echo ""
echo "For full documentation see CONTRIBUTING.md"
