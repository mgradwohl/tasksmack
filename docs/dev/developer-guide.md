# Developer Guide

This guide is a docs-site overview for contributors. `CONTRIBUTING.md` is the canonical source of truth for contributor setup, build/test workflow, and tooling; keep this page aligned with it.

For build-system details (presets, sanitizers, benchmarks, PGO), see [Build Instructions](build-instructions.md).

---

## Prerequisites

### Linux

| Tool | Version | Install |
|------|---------|---------|
| Clang + lld | 22 | `sudo apt install clang-22 lld-22` |
| libc++ / libc++abi | 22 | `sudo apt install libc++-22-dev libc++abi-22-dev` |
| clang-tidy | 22 | `sudo apt install clang-tidy-22` |
| clang-format | 22 | `sudo apt install clang-format-22` |
| CMake | 3.29+ (latest stable 3.x recommended) | `sudo apt install cmake` |
| Ninja | any | `sudo apt install ninja-build` |
| ccache | 4.9.1+ | `sudo apt install ccache` |
| Python 3 + jinja2 | 3.x | `sudo apt install python3 python3-jinja2` |
| FreeType | 2.13+ | `sudo apt install libfreetype6-dev` |
| llvm-profdata + llvm-cov | 22 | `sudo apt install llvm-22` |

**GPU monitoring libraries (optional):**

```bash
# NVIDIA: install driver from nvidia.com (includes libnvidia-ml.so)
# AMD: install ROCm SMI from amd.com/rocm (includes librocm_smi64.so)
# Intel: no package needed — the probe reads /sys/class/drm via sysfs (kernel DRM must be available)
```

**Full one-liner (Ubuntu/Debian):**

```bash
sudo apt install clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22 \
    libc++-22-dev libc++abi-22-dev cmake ninja-build ccache \
    python3 python3-jinja2 libfreetype6-dev
```

### Windows

| Tool | Install |
|------|---------|
| LLVM/Clang 22 | [releases.llvm.org](https://releases.llvm.org/) — set `LLVM_ROOT` env var |
| CMake 3.29+ | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |
| Python 3 + jinja2 | `winget install Python.Python.3.12` then `pip install jinja2` |
| ccache (optional) | `winget install ccache` |

---

## Quick Start

```bash
# 1. Clone
git clone https://github.com/mgradwohl/tasksmack.git
cd tasksmack

# 2. Install Python dependencies (includes pre-commit)
pip install -r requirements.txt

# 3. Install git pre-commit hooks (strongly recommended)
pre-commit install

# 4. Check all tools are discoverable
./tools/check-prereqs.sh      # Linux
pwsh tools/check-prereqs.ps1  # Windows
```

---

## VS Code Setup

### Recommended Extensions

- **clangd** (LLVM) — C++ language server (IntelliSense, diagnostics, go-to-definition)
- **CodeLLDB** — debugger
- **CMake Tools** — CMake integration

Install from the Extensions view (`Ctrl+Shift+X`) or via the CLI:

```bash
code --install-extension llvm-vs-code-extensions.vscode-clangd
code --install-extension vadimcn.vscode-lldb
code --install-extension ms-vscode.cmake-tools
```

### PATH and clangd Setup

The workspace `.vscode/settings.json` resolves `clangd` and `clang-format` from `PATH` automatically. Ensure LLVM tools are on `PATH` (the prerequisite scripts validate this).

**Linux — register unversioned alternatives** (only needed if only versioned binaries exist):

```bash
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-22 220
sudo update-alternatives --set clang-format /usr/bin/clang-format-22

sudo update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-22 220
sudo update-alternatives --set clangd /usr/bin/clangd-22
```

Re-run `./tools/check-prereqs.sh` to verify.

### Build Tasks

- **`Ctrl+Shift+B`** — default Debug build
- **Command Palette → "Tasks: Run Task"** — access all presets and tools

---

## Code Quality Tools

### clang-tidy (static analysis)

Run before submitting a PR. CI will fail on tidy warnings.

```bash
./tools/clang-tidy.sh debug        # Linux
pwsh tools/clang-tidy.ps1 debug    # Windows
```

The helper strips PCH flags from compile commands to avoid version-mismatch issues.

### clang-format (formatting)

Formatting is **required** before every PR commit. The pre-commit hook enforces this automatically.

```bash
./tools/clang-format.sh        # Linux — apply formatting
pwsh tools/clang-format.ps1    # Windows

./tools/check-format.sh        # Linux — check only (no changes)
pwsh tools/check-format.ps1    # Windows
```

### Include-What-You-Use (IWYU)

IWYU suggests `#include` additions and removals for cleaner dependencies.

```bash
./tools/iwyu.sh debug          # Analyse all files (report only)
./tools/iwyu.sh -v debug       # Verbose output
./tools/iwyu.sh --fix debug    # Apply fixes (review carefully!)
./tools/iwyu.sh src/Domain/ProcessModel.cpp  # Single file
```

IWYU runs in report-only mode in CI and does not block PRs. The project includes a `.iwyu.imp` mapping file for project-specific rules.

---

## Pre-commit Hooks

Pre-commit hooks run automatically on every `git commit` and catch issues before CI sees them.

### What Gets Checked

| Hook | Purpose |
|------|---------|
| `clang-format` | C++ code formatting (`.clang-format`) |
| `trailing-whitespace` | Remove trailing whitespace |
| `end-of-file-fixer` | Ensure files end with a newline |
| `mixed-line-ending` | Normalise line endings to LF |
| `check-yaml` | Validate YAML syntax |
| `check-json` | Validate JSON syntax |
| `check-added-large-files` | Prevent files > 500 KB |
| `check-merge-conflict` | Detect merge conflict markers |
| `shellcheck` | Lint shell scripts |

### Bypassing (Emergency Only)

```bash
git commit --no-verify
```

Use this sparingly — CI will still catch any issues.

---

## Coding Standards Summary

Full details are in the [architecture docs](architecture.md). Key rules:

### Layer Rules

- **Platform** → stateless reads of raw OS counters only; no computed values
- **Domain** → delta/rate computation, history, snapshots; no OS calls
- **UI** → renders snapshots via ImGui; never calls Platform directly
- **Core** → application lifecycle, SDL3, OpenGL context, `PathService`
- **App / Panels** → composition root; the only place `Platform::make*Probe()` is called

### Naming

| Symbol | Convention | Example |
|--------|-----------|---------|
| Classes | `PascalCase` | `ProcessModel` |
| Functions | `camelCase` | `refresh()` |
| Member variables | `m_camelCase` | `m_history` |
| Constants | `UPPER_SNAKE_CASE` | `DEFAULT_REFRESH_MS` |

### Include Order

1. Matching header (`.cpp` files)
2. Project headers (`src/`, `tests/`), alphabetical
3. Third-party headers (`<spdlog/…>`, `<imgui.h>`), alphabetical
4. Standard library headers, alphabetical

Separate each group with a blank line. Use `#pragma once` in all headers.

### C++23 Patterns to Use

- `std::string::contains()`, `starts_with()` — prefer over `find()`
- `std::ranges` / `std::views` — prefer over raw loops
- `std::format` / `std::print` — prefer over `fmt::format`
- `std::jthread` with `std::stop_token` — for background threads
- `std::optional`, `std::span`, `std::string_view` — for safer APIs
- `enum class EnumName : std::uint8_t` — prefer over plain `enum`
- `[[nodiscard]]` — on functions whose return value must not be ignored

### C++23 Patterns to Avoid

- `using namespace std` in headers
- C-style arrays, `char*`, `malloc`/`free`, raw pointers
- C-style casts (`(int)x`) — use `static_cast` with a comment when a cast is unavoidable
- Deprecated standard library features

### Rule of 5

If a class defines or deletes *any* of: destructor, copy constructor, copy assignment, move constructor, move assignment — it must define or delete **all five**.

Prefer smart pointers and RAII to avoid needing a custom destructor at all.

---

## Writing Tests

Tests live under `tests/` mirroring the `src/` layout. The framework is Google Test.

### MockProbes Builder Pattern

```cpp
#include "Mocks/MockProbes.h"

auto probe = std::make_unique<MockProcessProbe>();
probe->withProcess(123, "my_process")
     .withCpuTime(123, 1000, 500)
     .withMemory(123, 4096 * 1024)
     .withState(123, 'R');
probe->setTotalCpuTime(100000);
```

### Test Organisation Template

```cpp
namespace Domain {
namespace {

// ========== Basic Operations ==========
TEST(ProcessModelTest, InitialStateIsEmpty) { /* ... */ }

// ========== CPU Calculation ==========
TEST(ProcessModelTest, CpuPercentFromDeltas) { /* ... */ }

} // namespace
} // namespace Domain
```

Key rules:

- Use `EXPECT_DOUBLE_EQ` for float comparisons, not `EXPECT_EQ`
- Define mock classes **outside** anonymous namespaces when used with `std::make_unique`
- Mock files live in `tests/Mocks/MockProbes.h`

---

## Coverage

Coverage reports are written to `coverage/` (gitignored).

```bash
# Linux
./tools/coverage.sh            # Generate HTML report
./tools/coverage.sh --open     # Generate and open in browser

# Windows
pwsh tools/coverage.ps1
pwsh tools/coverage.ps1 -OpenReport
```

CI heavy-checks also publish a coverage summary and warn when coverage drops below the configured threshold.

---

## Packaging (CPack)

```bash
# Linux
cmake --preset release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G ZIP

# Windows
cmake --preset win-release
cmake --build --preset win-release
cpack --config build/win-release/CPackConfig.cmake -G ZIP
```

| Generator | Platform | Output |
|-----------|----------|--------|
| `ZIP` | All | `.zip` archive |
| `TGZ` | All | `.tar.gz` archive |
| `DEB` | Linux | Debian `.deb` package |
| `RPM` | Linux | Red Hat `.rpm` package |
| `NSIS` | Windows | `.exe` installer |

Packages are written to `dist/`.

---

## CI / Build Status

The table below shows live build status for the `main` branch. The CI badge at the top of the [README](https://github.com/mgradwohl/tasksmack#readme) reflects PR validation; the per-platform badges here reflect ongoing main-branch health.

| Platform | Debug | Release |
|----------|-------|---------|
| Linux | [![Linux Debug](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-linux-debug.yml?branch=main&style=flat-square&label=Linux%20Debug)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-linux-debug.yml) | [![Linux Release](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-linux-release.yml?branch=main&style=flat-square&label=Linux%20Release)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-linux-release.yml) |
| Windows | [![Windows Debug](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-windows-debug.yml?branch=main&style=flat-square&label=Windows%20Debug)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-windows-debug.yml) | [![Windows Release](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-windows-release.yml?branch=main&style=flat-square&label=Windows%20Release)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-windows-release.yml) |

Additional checks on `main`:

| Check | Status |
|-------|--------|
| Sanitizers (ASan/TSan/UBSan) | [![Sanitizers](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/sanitizers.yml?branch=main&style=flat-square&label=Sanitizers)](https://github.com/mgradwohl/tasksmack/actions/workflows/sanitizers.yml) |
| Static Analysis (clang-tidy) | [![Static Analysis](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/static-analysis.yml?branch=main&style=flat-square&label=Static%20Analysis)](https://github.com/mgradwohl/tasksmack/actions/workflows/static-analysis.yml) |
| CodeQL | [![CodeQL](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/codeql.yml?branch=main&style=flat-square&label=CodeQL)](https://github.com/mgradwohl/tasksmack/actions/workflows/codeql.yml) |
| OSV Dependency Scan | [![OSV Scan](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/osv-scanner.yml?branch=main&style=flat-square&label=OSV%20Scan)](https://github.com/mgradwohl/tasksmack/actions/workflows/osv-scanner.yml) |
| Pre-commit hooks | [![Pre-commit](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/pre-commit.yml?branch=main&style=flat-square&label=Pre-commit)](https://github.com/mgradwohl/tasksmack/actions/workflows/pre-commit.yml) |
| Coverage | [![Codecov](https://img.shields.io/codecov/c/github/mgradwohl/tasksmack?style=flat-square)](https://codecov.io/gh/mgradwohl/tasksmack) |
| OpenSSF Scorecard | [![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/mgradwohl/tasksmack/badge)](https://securityscorecards.dev/viewer/?uri=github.com/mgradwohl/tasksmack) |

---

## Contributing Workflow

1. **Check for existing issues** — search GitHub Issues before opening a new one.
2. **Use issue templates** — Bug Report and Feature Request templates are available.
3. **Fork + branch** — work on a feature branch, keep commits focused.
4. **Write tests** — new behaviour should be covered; avoid removing existing tests.
5. **Run the full local check** before pushing:
   ```bash
   pre-commit run --all-files
   cmake --preset debug && cmake --build --preset debug
   ctest --preset debug
   ./tools/clang-tidy.sh debug
   ```
6. **Open a PR** using the PR template checklist.
7. Security issues: report privately per [SECURITY.md](https://github.com/mgradwohl/tasksmack/blob/main/SECURITY.md).
