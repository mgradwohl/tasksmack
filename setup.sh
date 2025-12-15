#!/usr/bin/env bash
#
# Project Setup Script
# Checks prerequisites, installs them if needed, then renames placeholder
# "MyProject" to your project name throughout the codebase.
#
# Usage: ./setup.sh --name "YourProjectName" [--author "Your Name"]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Defaults
PROJECT_NAME=""
AUTHOR_NAME=""
VERBOSE=false
SKIP_PREREQS=false

# Required versions (minimum)
MIN_CMAKE_VERSION="3.28"
MIN_CLANG_VERSION="22"
MIN_CCACHE_VERSION="4.9.1"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

usage() {
    cat <<EOF
Usage: $(basename "$0") --name "YourProjectName" [OPTIONS]

Rename this template project to your chosen name.

Required:
  --name, -n NAME       Your project name (e.g., "AwesomeApp")

Optional:
  --author, -a NAME     Author name for documentation
  --skip-prereqs        Skip prerequisite checking
  --verbose, -v         Show detailed progress
  --help, -h            Show this help

Examples:
  $(basename "$0") --name "MyCoolProject"
  $(basename "$0") -n "DataProcessor" -a "Jane Doe" -v

Prerequisites (automatically checked/installed):
  - Clang ${MIN_CLANG_VERSION}+
  - CMake ${MIN_CMAKE_VERSION}+
  - Ninja
  - lld (LLVM linker)
  - ccache ${MIN_CCACHE_VERSION}+
  - clang-tidy (static analysis)
  - clang-format (code formatting)
  - llvm (for coverage tools)

Note: This script will:
  - Check and install required prerequisites
  - Rename MyProject -> YourProjectName in all files
  - Rename MYPROJECT -> YOURPROJECTNAME (uppercase)
  - Rename myproject -> yourprojectname (lowercase)
  - Update CMakeLists.txt, VS Code configs, and documentation
  - Delete itself and setup.ps1 after completion
EOF
    exit 0
}

# Compare version strings: returns 0 if $1 >= $2
version_ge() {
    printf '%s\n%s\n' "$2" "$1" | sort -V -C
}

# Extract major.minor version from version string
extract_version() {
    echo "$1" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
}

# Check if a command exists
command_exists() {
    command -v "$1" &>/dev/null
}

# Get clang version
get_clang_version() {
    if command_exists clang; then
        clang --version 2>/dev/null | grep -oE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get cmake version
get_cmake_version() {
    if command_exists cmake; then
        cmake --version 2>/dev/null | grep -oE 'cmake version [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get ccache version
get_ccache_version() {
    if command_exists ccache; then
        ccache --version 2>/dev/null | grep -oE 'ccache version [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
    else
        echo ""
    fi
}

# Get ninja version
get_ninja_version() {
    if command_exists ninja; then
        ninja --version 2>/dev/null | head -1
    else
        echo ""
    fi
}

# Check if lld is available
check_lld() {
    if command_exists ld.lld; then
        ld.lld --version 2>/dev/null | head -1
    elif command_exists lld; then
        lld --version 2>/dev/null | head -1
    else
        echo ""
    fi
}

# Check if llvm tools are available (for coverage)
check_llvm_tools() {
    if command_exists llvm-profdata && command_exists llvm-cov; then
        llvm-profdata show --version 2>/dev/null | grep -oE 'LLVM version [0-9]+' | head -1 || echo "available"
    else
        echo ""
    fi
}

# Check clang-tidy
check_clang_tidy() {
    if command_exists clang-tidy; then
        clang-tidy --version 2>/dev/null | grep -oE 'LLVM version [0-9]+' | grep -oE '[0-9]+' | head -1 || echo "available"
    else
        echo ""
    fi
}

# Check clang-format
check_clang_format() {
    if command_exists clang-format; then
        clang-format --version 2>/dev/null | grep -oE 'clang-format version [0-9]+' | grep -oE '[0-9]+' | head -1 || echo "available"
    else
        echo ""
    fi
}

# Detect package manager
detect_package_manager() {
    if command_exists apt-get; then
        echo "apt"
    elif command_exists dnf; then
        echo "dnf"
    elif command_exists yum; then
        echo "yum"
    elif command_exists pacman; then
        echo "pacman"
    elif command_exists brew; then
        echo "brew"
    else
        echo "unknown"
    fi
}

# Install prerequisites using detected package manager
install_prerequisites() {
    local pkg_manager
    pkg_manager=$(detect_package_manager)
    
    info "Detected package manager: $pkg_manager"
    
    case "$pkg_manager" in
        apt)
            info "Adding LLVM repository for latest clang..."
            # Check if we need to add the LLVM repo
            if ! apt-cache policy clang-22 2>/dev/null | grep -q "Candidate:"; then
                warn "clang-22 not in default repos, adding LLVM apt repository..."
                sudo apt-get update
                sudo apt-get install -y wget gnupg software-properties-common
                wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
                # Detect Ubuntu/Debian codename
                if [[ -f /etc/os-release ]]; then
                    . /etc/os-release
                    case "$ID" in
                        ubuntu)
                            sudo add-apt-repository -y "deb http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME} main"
                            ;;
                        debian)
                            sudo add-apt-repository -y "deb http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME} main"
                            ;;
                    esac
                fi
                sudo apt-get update
            fi
            
            info "Installing prerequisites via apt..."
            sudo apt-get install -y clang clang-tidy clang-format lld llvm cmake ninja-build ccache
            
            # Try to install versioned packages if available
            if apt-cache show clang-22 &>/dev/null; then
                sudo apt-get install -y clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22
            fi
            ;;
        dnf|yum)
            info "Installing prerequisites via $pkg_manager..."
            sudo "$pkg_manager" install -y clang clang-tools-extra lld llvm cmake ninja-build ccache
            ;;
        pacman)
            info "Installing prerequisites via pacman..."
            sudo pacman -S --noconfirm clang lld llvm cmake ninja ccache
            # clang-tidy and clang-format are included in the clang package on Arch
            ;;
        brew)
            info "Installing prerequisites via brew..."
            brew install llvm cmake ninja ccache
            warn "Note: You may need to add LLVM to your PATH:"
            warn "  export PATH=\"\$(brew --prefix llvm)/bin:\$PATH\""
            ;;
        *)
            error "Unknown package manager. Please install manually:"
            error "  - Clang ${MIN_CLANG_VERSION}+"
            error "  - CMake ${MIN_CMAKE_VERSION}+"
            error "  - Ninja"
            error "  - lld"
            error "  - ccache ${MIN_CCACHE_VERSION}+"
            error "  - llvm (for coverage: llvm-profdata, llvm-cov)"
            return 1
            ;;
    esac
}

# Suggest update-alternatives fix for version issues
suggest_alternatives_fix() {
    local tool="$1"
    local current_version="$2"
    local required_version="$3"
    
    echo ""
    warn "The '$tool' in your PATH reports version $current_version, but $required_version+ is required."
    echo ""
    
    case "$tool" in
        clang|clang++)
            info "Try one of these fixes:"
            echo "  1. Use update-alternatives (recommended):"
            echo "     sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-22 100"
            echo "     sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-22 100"
            echo "     sudo update-alternatives --config clang"
            echo ""
            echo "  2. Or add to your PATH (in ~/.bashrc or ~/.profile):"
            echo "     export PATH=\"/usr/lib/llvm-22/bin:\$PATH\""
            echo ""
            echo "  3. Or use environment variables:"
            echo "     export CC=clang-22"
            echo "     export CXX=clang++-22"
            ;;
        cmake)
            info "Try one of these fixes:"
            echo "  1. Install newer cmake via pip:"
            echo "     pip install --user cmake"
            echo ""
            echo "  2. Or download from cmake.org and add to PATH"
            echo ""
            echo "  3. Or use snap:"
            echo "     sudo snap install cmake --classic"
            ;;
        ccache)
            info "Try one of these fixes:"
            echo "  1. Install from source or newer package:"
            echo "     https://ccache.dev/download.html"
            echo ""
            echo "  2. Or via pip:"
            echo "     pip install --user ccache"
            ;;
    esac
}

# Main prerequisite check function
check_prerequisites() {
    echo ""
    info "========================================"
    info "  Checking Prerequisites"
    info "========================================"
    echo ""
    
    local all_ok=true
    local needs_install=false
    
    # Check Clang
    local clang_ver
    clang_ver=$(get_clang_version)
    if [[ -z "$clang_ver" ]]; then
        warn "Clang: NOT INSTALLED"
        needs_install=true
    elif [[ "$clang_ver" -lt "${MIN_CLANG_VERSION%%.*}" ]]; then
        warn "Clang: $clang_ver (required: ${MIN_CLANG_VERSION}+)"
        needs_install=true
    else
        success "Clang: $clang_ver"
    fi
    
    # Check CMake
    local cmake_ver
    cmake_ver=$(get_cmake_version)
    if [[ -z "$cmake_ver" ]]; then
        warn "CMake: NOT INSTALLED"
        needs_install=true
    elif ! version_ge "$cmake_ver" "$MIN_CMAKE_VERSION"; then
        warn "CMake: $cmake_ver (required: ${MIN_CMAKE_VERSION}+)"
        needs_install=true
    else
        success "CMake: $cmake_ver"
    fi
    
    # Check Ninja
    local ninja_ver
    ninja_ver=$(get_ninja_version)
    if [[ -z "$ninja_ver" ]]; then
        warn "Ninja: NOT INSTALLED"
        needs_install=true
    else
        success "Ninja: $ninja_ver"
    fi
    
    # Check lld
    local lld_ver
    lld_ver=$(check_lld)
    if [[ -z "$lld_ver" ]]; then
        warn "lld: NOT INSTALLED"
        needs_install=true
    else
        success "lld: available"
    fi
    
    # Check ccache
    local ccache_ver
    ccache_ver=$(get_ccache_version)
    if [[ -z "$ccache_ver" ]]; then
        warn "ccache: NOT INSTALLED"
        needs_install=true
    elif ! version_ge "$ccache_ver" "$MIN_CCACHE_VERSION"; then
        warn "ccache: $ccache_ver (required: ${MIN_CCACHE_VERSION}+)"
        needs_install=true
    else
        success "ccache: $ccache_ver"
    fi
    
    # Check LLVM tools (for coverage)
    local llvm_tools
    llvm_tools=$(check_llvm_tools)
    if [[ -z "$llvm_tools" ]]; then
        warn "llvm-profdata/llvm-cov: NOT INSTALLED (needed for coverage)"
        needs_install=true
    else
        success "llvm tools: available (for coverage)"
    fi
    
    # Check clang-tidy
    local clang_tidy
    clang_tidy=$(check_clang_tidy)
    if [[ -z "$clang_tidy" ]]; then
        warn "clang-tidy: NOT INSTALLED (needed for static analysis)"
        needs_install=true
    else
        success "clang-tidy: $clang_tidy"
    fi
    
    # Check clang-format
    local clang_format
    clang_format=$(check_clang_format)
    if [[ -z "$clang_format" ]]; then
        warn "clang-format: NOT INSTALLED (needed for code formatting)"
        needs_install=true
    else
        success "clang-format: $clang_format"
    fi
    
    # Install if needed
    if $needs_install; then
        echo ""
        info "Some prerequisites are missing or outdated."
        read -p "Would you like to install/update them now? [Y/n] " -n 1 -r
        echo ""
        if [[ ! $REPLY =~ ^[Nn]$ ]]; then
            install_prerequisites
            echo ""
            info "Re-checking prerequisites after installation..."
            echo ""
            verify_prerequisites
            return $?
        else
            error "Cannot continue without prerequisites."
            return 1
        fi
    fi
    
    return 0
}

# Verify prerequisites meet version requirements (after installation)
verify_prerequisites() {
    local all_ok=true
    
    # Re-check Clang
    local clang_ver
    clang_ver=$(get_clang_version)
    if [[ -z "$clang_ver" ]]; then
        error "Clang: STILL NOT INSTALLED"
        all_ok=false
    elif [[ "$clang_ver" -lt "${MIN_CLANG_VERSION%%.*}" ]]; then
        error "Clang: $clang_ver (required: ${MIN_CLANG_VERSION}+)"
        suggest_alternatives_fix "clang" "$clang_ver" "$MIN_CLANG_VERSION"
        all_ok=false
    else
        success "Clang: $clang_ver"
    fi
    
    # Re-check CMake
    local cmake_ver
    cmake_ver=$(get_cmake_version)
    if [[ -z "$cmake_ver" ]]; then
        error "CMake: STILL NOT INSTALLED"
        all_ok=false
    elif ! version_ge "$cmake_ver" "$MIN_CMAKE_VERSION"; then
        error "CMake: $cmake_ver (required: ${MIN_CMAKE_VERSION}+)"
        suggest_alternatives_fix "cmake" "$cmake_ver" "$MIN_CMAKE_VERSION"
        all_ok=false
    else
        success "CMake: $cmake_ver"
    fi
    
    # Re-check Ninja
    local ninja_ver
    ninja_ver=$(get_ninja_version)
    if [[ -z "$ninja_ver" ]]; then
        error "Ninja: STILL NOT INSTALLED"
        all_ok=false
    else
        success "Ninja: $ninja_ver"
    fi
    
    # Re-check lld
    local lld_ver
    lld_ver=$(check_lld)
    if [[ -z "$lld_ver" ]]; then
        error "lld: STILL NOT INSTALLED"
        all_ok=false
    else
        success "lld: available"
    fi
    
    # Re-check ccache
    local ccache_ver
    ccache_ver=$(get_ccache_version)
    if [[ -z "$ccache_ver" ]]; then
        error "ccache: STILL NOT INSTALLED"
        all_ok=false
    elif ! version_ge "$ccache_ver" "$MIN_CCACHE_VERSION"; then
        error "ccache: $ccache_ver (required: ${MIN_CCACHE_VERSION}+)"
        suggest_alternatives_fix "ccache" "$ccache_ver" "$MIN_CCACHE_VERSION"
        all_ok=false
    else
        success "ccache: $ccache_ver"
    fi
    
    # Re-check LLVM tools
    local llvm_tools
    llvm_tools=$(check_llvm_tools)
    if [[ -z "$llvm_tools" ]]; then
        warn "llvm-profdata/llvm-cov: NOT INSTALLED (coverage will not work)"
        # Don't fail for this - it's optional
    else
        success "llvm tools: available"
    fi
    
    # Re-check clang-tidy
    local clang_tidy
    clang_tidy=$(check_clang_tidy)
    if [[ -z "$clang_tidy" ]]; then
        error "clang-tidy: STILL NOT INSTALLED"
        all_ok=false
    else
        success "clang-tidy: $clang_tidy"
    fi
    
    # Re-check clang-format
    local clang_format
    clang_format=$(check_clang_format)
    if [[ -z "$clang_format" ]]; then
        error "clang-format: STILL NOT INSTALLED"
        all_ok=false
    else
        success "clang-format: $clang_format"
    fi
    
    if ! $all_ok; then
        echo ""
        error "Some prerequisites still don't meet requirements."
        error "Please fix the issues above and re-run this script."
        return 1
    fi
    
    echo ""
    success "All prerequisites verified!"
    return 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--name)
            PROJECT_NAME="$2"
            shift 2
            ;;
        -a|--author)
            AUTHOR_NAME="$2"
            shift 2
            ;;
        --skip-prereqs)
            SKIP_PREREQS=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Error: Unknown argument: $1" >&2
            usage
            ;;
    esac
done

# Validate
if [[ -z "$PROJECT_NAME" ]]; then
    echo "Error: --name is required" >&2
    usage
fi

echo ""
echo "========================================"
echo "  MyProject Setup Script"
echo "========================================"

# Check prerequisites first (unless skipped)
if ! $SKIP_PREREQS; then
    if ! check_prerequisites; then
        exit 1
    fi
fi

# Derive variants
PROJECT_UPPER=$(echo "$PROJECT_NAME" | tr '[:lower:]' '[:upper:]')
PROJECT_LOWER=$(echo "$PROJECT_NAME" | tr '[:upper:]' '[:lower:]')

echo ""
info "========================================"
info "  Configuring Project"
info "========================================"
echo ""
info "Setting up project: $PROJECT_NAME"
info "  Uppercase: $PROJECT_UPPER"
info "  Lowercase: $PROJECT_LOWER"
[[ -n "$AUTHOR_NAME" ]] && info "  Author: $AUTHOR_NAME"
echo ""

# Files to process (excluding .git, build, and this script)
find_files() {
    find "$SCRIPT_DIR" -type f \
        ! -path "*/.git/*" \
        ! -path "*/build/*" \
        ! -path "*/dist/*" \
        ! -path "*/coverage/*" \
        ! -name "setup.sh" \
        ! -name "setup.ps1" \
        -print0
}

# Count replacements
COUNT=0

while IFS= read -r -d '' file; do
    # Skip binary files
    if file "$file" | grep -q "text"; then
        MODIFIED=false
        
        # Replace MyProject -> ProjectName
        if grep -q "MyProject" "$file" 2>/dev/null; then
            sed -i "s/MyProject/$PROJECT_NAME/g" "$file"
            MODIFIED=true
        fi
        
        # Replace MYPROJECT -> PROJECTNAME
        if grep -q "MYPROJECT" "$file" 2>/dev/null; then
            sed -i "s/MYPROJECT/$PROJECT_UPPER/g" "$file"
            MODIFIED=true
        fi
        
        # Replace myproject -> projectname
        if grep -q "myproject" "$file" 2>/dev/null; then
            sed -i "s/myproject/$PROJECT_LOWER/g" "$file"
            MODIFIED=true
        fi
        
        # Update author if provided
        if [[ -n "$AUTHOR_NAME" ]] && grep -q "Your Name" "$file" 2>/dev/null; then
            sed -i "s/Your Name/$AUTHOR_NAME/g" "$file"
            MODIFIED=true
        fi
        
        if $MODIFIED; then
            COUNT=$((COUNT + 1))
            if $VERBOSE; then
                echo "  Updated: ${file#$SCRIPT_DIR/}"
            fi
        fi
    fi
done < <(find_files)

echo ""
success "Updated $COUNT files."

# Clean up setup scripts
info "Removing setup scripts..."
rm -f "$SCRIPT_DIR/setup.sh" "$SCRIPT_DIR/setup.ps1"

echo ""
success "Setup complete! Your project '$PROJECT_NAME' is ready."
echo ""
echo "Next steps:"
echo "  1. Review the changes: git diff"
echo "  2. Build the project: cmake --preset debug && cmake --build --preset debug"
echo "  3. Run tests: ctest --preset debug"
echo "  4. Commit: git add -A && git commit -m 'Initial project setup'"
