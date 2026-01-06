# Contributing to TaskSmack

Thanks for contributing!

This document is the single source of truth for developer setup and workflows (build/test/format/lint/packaging).

## Documentation

To avoid duplication and doc drift, these are the canonical docs:

- [README.md](README.md): user-facing features (with a small contributor pointer to this file)
- [CONTRIBUTING.md](CONTRIBUTING.md): contributor workflow (this file)
- [tasksmack.md](tasksmack.md): architecture + engineering notes (including process/metrics implementation notes)
- [completed-features.md](completed-features.md): canonical shipped-features list
- [docs/test-coverage-summary.md](docs/test-coverage-summary.md): test coverage analysis (executive summary)
- [docs/test-coverage-analysis.md](docs/test-coverage-analysis.md): comprehensive test coverage review (full report)
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
    - `sudo apt install clang-22 lld-22 libc++-22-dev libc++abi-22-dev`
    - `<print>` from C++23 requires a C++23-ready standard library (libc++ 22)
- CMake 3.28+ (4.2.1+ recommended)
- Ninja
- lld (LLVM linker)
- clang-tidy and clang-format
- ccache 4.9.1+ (recommended for faster rebuilds)
- llvm-profdata and llvm-cov (coverage)
- Python 3 + jinja2 (required for GLAD OpenGL loader generation)
- FreeType 2.13+ (font rendering library) - typically auto-detected from system or fetched if not found
- **Optional GPU monitoring libraries:**
  - NVIDIA drivers with NVML (libnvidia-ml.so) for NVIDIA GPU support
  - ROCm SMI library (librocm_smi64.so) for AMD GPU support
  - libdrm-dev for basic Intel GPU enumeration

Example (Ubuntu/Debian):

```bash
sudo apt install clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22 cmake ninja-build ccache python3 python3-jinja2 libfreetype6-dev

# Optional: For GPU monitoring
sudo apt install libdrm-dev          # Intel GPU enumeration
# NVIDIA drivers (download from nvidia.com)
# ROCm SMI (download from amd.com/rocm)
```

### Windows Pre-Requisites

- LLVM/Clang 21+ (includes clang-tidy, clang-format, lld, llvm-cov)
- `LLVM_ROOT` environment variable set
- CMake 3.28+
- Ninja
- ccache 4.9.1+ (optional but recommended)
- Python 3 + jinja2 (required for GLAD OpenGL loader generation)
- FreeType 2.13+ (font rendering library) - typically auto-detected or fetched if not found

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
- **check-yaml**: Validate YAML syntax
- **check-json**: Validate JSON syntax
- **check-added-large-files**: Prevent large files (>500KB)
- **check-merge-conflict**: Detect merge conflict markers
- **shellcheck**: Lint shell scripts

### Bypassing Hooks (Emergency Only)

If you need to commit without running hooks (not recommended):

```bash
git commit --no-verify
```

## Constants

- Shared sampling defaults/guardrails live in `src/Domain/SamplingConfig.h` (refresh interval ms, history seconds, clamp helpers). Reuse them instead of hardcoding new literals.
- Prefer `constexpr` for project constants. Keep platform-required macros (`WIN32_LEAN_AND_MEAN`, `GLFW_INCLUDE_NONE`, etc.) as `#define`.

## Build

This repo uses CMake Presets; list them with:

```bash
cmake --list-presets
```

### CPU Compatibility

The `optimized` and `win-optimized` presets target the x86-64-v3 microarchitecture, which requires AVX2 support (Haswell 2013+ or Excavator 2015+ CPUs). If you encounter "Illegal instruction" errors, your CPU may not support these instructions.

For broader compatibility, use:
- `release-compatible` (Linux) or `win-release-compatible` (Windows) for x86-64-v2 (2009+)
- `release` (Linux) or `win-release` (Windows) for default compiler optimizations

You can also customize the target microarchitecture by setting the `TASKSMACK_MARCH` CMake variable:

```bash
cmake --preset release -DTASKSMACK_MARCH=native  # Optimize for your specific CPU
cmake --preset release -DTASKSMACK_MARCH=x86-64-v2  # Target 2009+ CPUs
```

### Common Presets

| Preset (Linux) | Preset (Windows) | Description |
|----------------|------------------|-------------|
| `debug` | `win-debug` | Debug symbols, no optimization, security hardening |
| `relwithdebinfo` | `win-relwithdebinfo` | Debug symbols + optimization |
| `release` | `win-release` | Optimized, no debug symbols |
| `release-compatible` | `win-release-compatible` | Release build for older CPUs (x86-64-v2, 2009+) |
| `optimized` | `win-optimized` | LTO, march=x86-64-v3, stripped (Haswell 2013+) |
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

### Static Analysis (run first)

```bash
./tools/clang-tidy.sh debug        # Linux
pwsh tools/clang-tidy.ps1 debug    # Windows
```

Note: the build uses precompiled headers (PCH). The clang-tidy helper strips PCH flags from the compile commands to avoid version mismatch issues.

### Include-What-You-Use (IWYU)

IWYU analyzes `#include` directives and suggests additions/removals for cleaner dependencies:

```bash
# Analyze all files (report only)
./tools/iwyu.sh debug

# Analyze with verbose output
./tools/iwyu.sh -v debug

# Apply suggested fixes (use with caution - review changes!)
./tools/iwyu.sh --fix debug

# Analyze specific file
./tools/iwyu.sh src/Domain/ProcessModel.cpp
```

**Installation:**
```bash
# Ubuntu/Debian
sudo apt install iwyu

# macOS
brew install include-what-you-use
```

**Notes:**
- IWYU suggestions are advisory and may not always be appropriate.
- CI runs IWYU in report-only mode (does not block PRs).
- **CI trigger:** The IWYU CI job (`include-analysis`) is manual-only and will not run automatically on PRs. To run it, manually trigger the CI workflow via the Actions tab using "workflow_dispatch".
- The project includes a `.iwyu.imp` mapping file for project-specific rules.
- To run via pre-commit: `pre-commit run iwyu --hook-stage manual`.
- **Version compatibility:** IWYU works best when built against the same clang version as your project, but minor version differences usually work. The system package (`apt install iwyu`) may produce warnings if versions mismatch. This is expected - rely on CI for accurate results.

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

## Benchmarks

TaskSmack includes a benchmark suite using [Google Benchmark](https://github.com/google/benchmark) for tracking performance regressions and identifying hot paths.

### Running Benchmarks

```bash
# Linux
cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/bin/TaskSmackBenchmarks

# Windows
cmake --preset win-benchmark
cmake --build --preset win-benchmark
.\build\win-benchmark\bin\TaskSmackBenchmarks.exe
```

### Benchmark Output

By default, benchmarks output to console. You can also:

```bash
# JSON output for comparison
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_format=json > baseline.json

# Compare against baseline
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_format=json > current.json
# Use benchmark_compare.py from google/benchmark for comparison
```

### Available Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_History_*` | Ring buffer operations (push, access, copyTo) |
| `BM_History_MemoryFootprint` | Memory usage tracking for history buffers |
| `BM_ProcessModel_*` | Process enumeration and snapshot computation |
| `BM_ProcessModel_MemoryGrowth` | Memory growth over repeated refresh cycles |
| `BM_ProcessProbe_Enumerate` | Raw OS API performance |
| `BM_Format_*` | UI formatting functions |

### Memory Tracking

Benchmarks include memory tracking to catch allocation regressions. Memory counters are reported alongside timing:

```bash
# Run with tabular counters to see memory metrics
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_counters_tabular=true

# Filter to memory-focused benchmarks
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_filter=Memory
```

**Memory counters reported:**
- `rss_mb` - Resident Set Size (physical memory) at end of benchmark
- `heap_mb` - Heap (data segment) size
- `peak_rss_mb` - High water mark for RSS
- `rss_delta_kb` - RSS change during benchmark
- `bytes_per_iter` - Memory growth per iteration (should be ~0 for stable code)

On Linux, memory tracking uses `/proc/self/status` for zero-overhead measurement; these memory counters are currently only available on Linux builds.

## Performance Profiling

The `profile` and `win-profile` presets are optimized for profiling with frame pointers preserved.

### Linux (perf)

```bash
# Build with profiling preset
cmake --preset profile
cmake --build --preset profile

# Run with perf record
perf record -g ./build/profile/bin/TaskSmack

# Analyze
perf report -g

# Generate flamegraph (requires flamegraph.pl)
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

### Linux (perf stat for quick metrics)

```bash
# Quick performance counters
perf stat ./build/profile/bin/TaskSmackBenchmarks --benchmark_filter=BM_ProcessModel_Refresh
```

### macOS (Instruments)

```bash
# Build with profile preset
cmake --preset profile
cmake --build --preset profile

# Open in Instruments
open -a Instruments ./build/profile/bin/TaskSmack

# Or use command line
xcrun xctrace record --template 'Time Profiler' --launch -- ./build/profile/bin/TaskSmack
```

### Windows (ETW/VTune)

```powershell
# Build with profiling preset
cmake --preset win-profile
cmake --build --preset win-profile

# Use Windows Performance Analyzer (WPA) or Intel VTune
# For VTune:
vtune -collect hotspots -- .\build\win-profile\bin\TaskSmack.exe
```

### Compile-Time Profiling (-ftime-trace)

To identify slow headers and compilation bottlenecks:

```bash
# Add -ftime-trace to your build
cmake --preset debug -DCMAKE_CXX_FLAGS="-ftime-trace"
cmake --build --preset debug

# Each .cpp generates a .json trace file
# Open in Chrome's chrome://tracing or Perfetto
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

### Shared FetchContent cache

The shared FetchContent cache is **enabled by default** to reuse downloads across presets, reducing build times and bandwidth usage. The cache is stored at `.cache/fetchcontent/` in the project root.

To disable the cache:

```bash
cmake --preset debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
cmake --preset win-debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
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
- `iwyu-results`

### CI Artifacts (GitHub CLI)

```bash
gh run download <run-id> -n coverage-html-report
```

## Branching Strategy

The project uses the following branch patterns:

| Branch | Purpose | CI Runs |
|--------|---------|---------|
| `main` | Stable release branch | Yes |
| `feature/*` | Individual feature branches | On PR to main |
| `dev/*` | Integration branches for multi-PR epics | Yes |

### When to use `dev/*` branches

Use a `dev/` branch when working on a large feature that spans multiple PRs (an "epic"). For example:
- `dev/network-monitoring` - Collects multiple network-related PRs before merging to main
- `dev/gpu-support` - Integration branch for GPU monitoring features

Workflow:
1. Create `dev/epic-name` from `main`
2. Create feature branches and PR them into `dev/epic-name`
3. Once all features are complete and tested, PR `dev/epic-name` into `main`

For simple single-PR features, branch directly from `main` with a `feature/` prefix.

## Pull Request Process

1. Fork the repository
2. Create a feature branch (or target a `dev/*` branch for epic work)
3. Make your changes
4. Run clang-tidy: `./tools/clang-tidy.sh debug` (Linux) or `pwsh tools/clang-tidy.ps1 debug` (Windows)
5. Run formatting: `./tools/clang-format.sh` (Linux) or `pwsh tools/clang-format.ps1` (Windows)
6. Run pre-commit checks: `pre-commit run --all-files` (if installed)
7. Run tests
8. Open a PR and follow the checklist in the PR template: [.github/pull_request_template.md](.github/pull_request_template.md)

**Note:** If you installed pre-commit hooks (recommended), format checks run automatically on commit.

## Reporting Issues

Please use the issue templates:

- Bug Report: [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)
- Feature Request: [.github/ISSUE_TEMPLATE/feature_request.md](.github/ISSUE_TEMPLATE/feature_request.md)

## Security Issues

See [SECURITY.md](SECURITY.md) for responsible disclosure. Do not open public issues for security vulnerabilities.
