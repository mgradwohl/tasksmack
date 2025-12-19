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

- Clang 21+ recommended
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

## Build

This repo uses CMake Presets; list them with:

```bash
cmake --list-presets
```

### Common Presets

| Preset (Linux) | Preset (Windows) | Description |
|----------------|------------------|-------------|
| `debug` | `win-debug` | Debug symbols, no optimization, security hardening |
| `relwithdebinfo` | `win-relwithdebinfo` | Debug symbols + optimization |
| `release` | `win-release` | Optimized, no debug symbols |
| `optimized` | `win-optimized` | LTO, march=x86-64-v3, stripped |
| `coverage` | `win-coverage` | Debug + code coverage instrumentation |
| `asan-ubsan` | — | AddressSanitizer + UBSan (Linux only) |
| `tsan` | — | ThreadSanitizer (Linux only) |

### Build Commands

```bash
# Linux
cmake --preset debug
cmake --build --preset debug

# Windows
cmake --preset win-debug
cmake --build --preset win-debug
```

### Running

The application target is `TaskSmack`.

- Windows: the binary is under `build/win-debug/bin/TaskSmack.exe` (or your selected preset)
- Linux: the binary is under `build/debug/bin/TaskSmack` (or your selected preset)

## Test

```bash
# Linux
ctest --preset debug

# Windows
ctest --preset win-debug
```

### Integration tests

Integration tests live under `tests/Integration/` and are built into the main test target (so they run via the same `ctest --preset ...` commands). Some integration tests are OS-specific and are conditionally included/skipped depending on platform.

In practice, `tests/Integration/` includes both cross-platform tests and Linux-only tests (e.g., tests that validate `/proc` parsing). Those Linux-only tests are excluded from Windows builds.

To run only integration tests:

```bash
# Linux
ctest --preset debug -R Integration

# Windows
ctest --preset win-debug -R Integration
```

### Writing tests (mocks)

For unit tests that need process/system probe data, prefer the mocks in `tests/Mocks/MockProbes.h`. `MockProcessProbe` supports a fluent builder-style API:

```cpp
auto probe = std::make_unique<MockProcessProbe>();
probe->withProcess(123, "test_process").withCpuTime(123, 1000, 500).withMemory(123, 4096 * 1024).withState(123, 'R');
probe->setTotalCpuTime(100000);
```

## VS Code

Recommended extensions:

- clangd (LLVM)
- CodeLLDB
- CMake Tools

Build tasks are preconfigured:

- `Ctrl+Shift+B` runs the default Debug build
- Command Palette → “Tasks: Run Task” for other presets and tools

## Code Quality Tools

### Formatting (required before PRs)

```bash
./tools/clang-format.sh        # Linux
pwsh tools/clang-format.ps1    # Windows
```

Check formatting (no changes):

```bash
./tools/check-format.sh        # Linux
pwsh tools/check-format.ps1    # Windows
```

### Static Analysis

```bash
./tools/clang-tidy.sh debug        # Linux
pwsh tools/clang-tidy.ps1 debug    # Windows
```

Note: the build uses precompiled headers (PCH). The clang-tidy helper strips PCH flags from the compile commands to avoid version mismatch issues.

## Coverage

Coverage reports are written to `coverage/` (gitignored).

CI also publishes a coverage summary and may emit a warning if coverage is below the configured threshold.

```bash
# Linux
./tools/coverage.sh
./tools/coverage.sh --open
# Windows
pwsh tools/coverage.ps1
pwsh tools/coverage.ps1 -OpenReport
```

## Sanitizers (Linux only)

AddressSanitizer + UndefinedBehaviorSanitizer:

```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

ThreadSanitizer:

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

## Packaging (CPack)

Create distributable archives/installers with CPack:

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

Supported generators:

| Generator | Platform | Output |
|-----------|----------|--------|
| `ZIP` | All | .zip archive |
| `TGZ` | All | .tar.gz archive |
| `DEB` | Linux | Debian .deb package |
| `RPM` | Linux | Red Hat .rpm package |
| `NSIS` | Windows | .exe installer |

Packages are created in `dist/`.

## Version Header

The build auto-generates a `version.h` header at configure time with project version, build type, compiler info, and build timestamp.

Usage:

```cpp
#include "version.h"

spdlog::info("{} v{} ({} build)", tasksmack::Version::PROJECT_NAME, tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
spdlog::debug("Compiler: {} {}", tasksmack::Version::COMPILER_ID, tasksmack::Version::COMPILER_VERSION);
spdlog::debug("Built: {} {}", tasksmack::Version::BUILD_DATE, tasksmack::Version::BUILD_TIME);
```

The header is generated to `build/<preset>/generated/version.h`.

## Compiler Warnings

The project enables a comprehensive warning set tuned for Clang on Windows and Linux.

Key CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `TASKSMACK_ENABLE_WARNINGS` | `ON` | Enable extra warnings |
| `TASKSMACK_WARNINGS_AS_ERRORS` | `ON` | Treat warnings as errors |

To disable warnings-as-errors for local iteration:

```bash
cmake --preset debug -DTASKSMACK_WARNINGS_AS_ERRORS=OFF
```

## Build System Notes

Some choices are intentional (to keep the build predictable across Windows/Linux):

- CMake Presets are the source of truth for configurations
- FetchContent is used for dependencies (prefer `SYSTEM` to reduce third-party warning noise)
- Platform default C++ standard libraries are used (libstdc++ on Linux, MSVC STL on Windows)

Clang-tidy configuration is curated for signal/noise; see `.clang-tidy` for the current list of disabled checks. Work to re-enable selected checks is tracked in GitHub issues (#60, #61, #62, #63, #64).

## Adding Dependencies

Use CMake’s `FetchContent` for dependencies. Always use `SYSTEM` to suppress third-party warnings:

```cmake
FetchContent_Declare(
    mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM
)
FetchContent_MakeAvailable(mylib)

target_link_libraries(TaskSmack PRIVATE mylib)
```

### Optional: shared FetchContent cache

Enable the shared cache to reuse downloads across presets:

```bash
cmake --preset debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=ON
cmake --preset win-debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=ON
```

Override the cache dir with `TASKSMACK_FETCHCONTENT_CACHE_DIR` or `FETCHCONTENT_BASE_DIR`.

## CI/CD

GitHub Actions builds on Linux (Ubuntu 24.04) and Windows and runs:

- Build + tests
- Build + tests for debug and release configurations
- Sanitizers (Linux)
- clang-format check
- clang-tidy
- Coverage (coverage preset)

Dependabot updates GitHub Actions dependencies weekly.

### CI Artifacts (GitHub UI)

In Actions → workflow run → Artifacts, you may see:

- `coverage-html-report`
- `asan-ubsan-report`
- `tsan-report`
- `linux-test-results` / `windows-test-results`
- `clang-tidy-results`

### CI Artifacts (GitHub CLI)

```bash
gh run download <run-id> -n coverage-html-report
```

## Pull Request Process

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run formatting (required)
5. Run clang-tidy (recommended)
6. Run tests
7. Open a PR and follow the checklist in the PR template: [.github/pull_request_template.md](.github/pull_request_template.md)

## Reporting Issues

Please use the issue templates:

- Bug Report: [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)
- Feature Request: [.github/ISSUE_TEMPLATE/feature_request.md](.github/ISSUE_TEMPLATE/feature_request.md)

## Security Issues

See [SECURITY.md](SECURITY.md) for responsible disclosure. Do not open public issues for security vulnerabilities.
