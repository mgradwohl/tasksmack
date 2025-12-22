# Contributing to TaskSmack

Thanks for contributing!

This document is the single source of truth for developer setup and workflows (build/test/format/lint/packaging).

## Documentation

To avoid duplication and doc drift, these are the canonical docs:

- [README.md](README.md): user-facing features (with a small contributor pointer to this file)
- [CONTRIBUTING.md](CONTRIBUTING.md): contributor workflow (this file)
- [tasksmack.md](tasksmack.md): architecture + engineering notes (including process/metrics implementation notes)
- [completed-features.md](completed-features.md): canonical shipped-features list
- [.github/copilot-instructions.md](.github/copilot-instructions.md) and [.github/copilot-coding-agent-tips.md](.github/copilot-coding-agent-tips.md): agent guidance (also useful to contributors)

## Quick Start

```bash
git clone https://github.com/mgradwohl/tasksmack.git
cd tasksmack

# Install Python dependencies (including pre-commit)
pip install -r requirements.txt

# Set up pre-commit hooks (recommended)
pre-commit install
```

## Check Prerequisites

If you just want a quick check of your environment, run:

```bash
./tools/check-prereqs.sh    # Linux
.\tools\check-prereqs.ps1   # Windows
```

# Configure + build (Windows)
```cmake --preset win-debug
cmake --build --preset win-debug
```

# Run tests
```bash
ctest --preset win-debug
```

### Linux Pre-Requisites

- **Clang 22 + libc++/libc++abi 22 (matches CI)**
    - `sudo apt install clang-22 lld-22 g++-13 libc++-22-dev libc++abi-22-dev`
    - `<print>` from C++23 requires a C++23-ready standard library (libc++ 22)
- CMake 3.28+ (4.2.1+ recommended)
- Ninja
- lld (LLVM linker)
- clang-tidy and clang-format
- ccache 4.9.1+ (recommended for faster rebuilds)
- llvm-profdata and llvm-cov (coverage)
- Python 3 + jinja2 (required for GLAD OpenGL loader generation)

Example (Ubuntu/Debian):

```bash
sudo apt install clang-21 clang-tidy-21 clang-format-21 lld-21 llvm-21 cmake ninja-build ccache python3 python3-jinja2
```

### Windows Pre-Requisites

- LLVM/Clang 21+ (includes clang-tidy, clang-format, lld, llvm-cov)
- `LLVM_ROOT` environment variable set
- CMake 3.28+
- Ninja
- ccache 4.9.1+ (optional but recommended)
- Python 3 + jinja2 (required for GLAD OpenGL loader generation)

Install Python + jinja2:

```powershell
winget install Python.Python.3.12
pip install jinja2
```

## Pre-commit Hooks (Recommended)

Pre-commit hooks automatically check your code before each commit, catching formatting and style issues early. This is **strongly recommended** to avoid CI failures.

### Install

```bash
# Install pre-commit (one-time setup)
pip install pre-commit

# Install the git hooks (run from project root)
pre-commit install
```

Or install from requirements.txt:

```bash
pip install -r requirements.txt
pre-commit install
```

### Usage

Once installed, pre-commit hooks run automatically on `git commit`. To run manually on all files:

```bash
pre-commit run --all-files
```

### What Gets Checked

The hooks (configured in `.pre-commit-config.yaml`) include:

- **clang-format**: C++ code formatting (uses project's `.clang-format`)
- **trailing-whitespace**: Remove trailing whitespace
- **end-of-file-fixer**: Ensure files end with a newline
- **mixed-line-ending**: Normalize line endings to LF
