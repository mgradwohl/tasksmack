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
    - `sudo apt install clang-22 lld-22 libc++-22-dev libc++abi-22-dev`
    - `<print>` from C++23 requires a C++23-ready standard library (libc++ 22)
- CMake 3.29+ (4.x recommended)
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
  - Intel: no package needed — reads `/sys/class/drm` via sysfs (kernel DRM support is standard on all modern Linux kernels)

Example (Ubuntu/Debian):

```bash
sudo apt install clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22 cmake ninja-build ccache python3 python3-jinja2 libfreetype6-dev

# Optional: For GPU monitoring
# NVIDIA drivers (download from nvidia.com)
# ROCm SMI (download from amd.com/rocm)
# Intel: no package needed (reads /sys/class/drm via sysfs)
```

### Windows Pre-Requisites

- LLVM/Clang 22 (includes clang-tidy, clang-format, lld, llvm-cov, llvm-profdata)
- `LLVM_ROOT` environment variable set
- CMake 3.29+
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
- Prefer `constexpr` for project constants. Keep platform-required macros (`WIN32_LEAN_AND_MEAN`, etc.) as `#define`.

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

Workspace settings resolve `clangd` and `clang-format` from `PATH` so the same `.vscode/settings.json` works across Linux/WSL/Windows. Ensure LLVM tools are on `PATH` (the prerequisite scripts validate this). Only use user-level VS Code overrides if your local install path is nonstandard.

Before troubleshooting VS Code diagnostics, run the prerequisite check to confirm required tools are installed and discoverable on `PATH`:

```bash
./tools/check-prereqs.sh      # Linux
pwsh tools/check-prereqs.ps1  # Windows
```

If this check passes, `clangd`/`clang-format` should be auto-discovered by the workspace settings.

Linux note (durable PATH setup): if only versioned LLVM binaries exist (for example `clang-format-22`), register unversioned commands system-wide with `update-alternatives`:

```bash
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-22 220
sudo update-alternatives --set clang-format /usr/bin/clang-format-22

sudo update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-22 220
sudo update-alternatives --set clangd /usr/bin/clangd-22
```

Then re-run:

```bash
./tools/check-prereqs.sh
```

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
- **Version compatibility:** IWYU must be built against Clang 22+. The script checks the version and: **fails hard** in CI or when `--fix` is used (to prevent corrupting includes); **warns and continues** for local dry runs (so you can still see results). The system package (`apt install iwyu`) is typically built against an older Clang and will trigger this warning locally. For reliable local runs, build iwyu from source against Clang 22+: https://github.com/include-what-you-use/include-what-you-use

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

CI heavy checks also publish a coverage summary and may emit a warning if coverage is below the configured threshold.

```bash
# Linux
./tools/coverage.sh
./tools/coverage.sh --open
# Windows
pwsh tools/coverage.ps1
pwsh tools/coverage.ps1 -OpenReport
```

> **Note (Linux GPU mock tests):** The Linux GPU probe tests depend on mock shared libraries
> (`libnvidia-ml.so.1`, `librocm_smi64.so.6`) built into `build/<preset>/tests/mocks/`
> (e.g. `build/debug/tests/mocks/` or `build/coverage/tests/mocks/`).
> CTest sets `LD_LIBRARY_PATH` automatically via `ENVIRONMENT_MODIFICATION`, and
> `tools/coverage.sh` exports it before the direct binary run.  If you run the test binary
> directly (e.g. `./build/debug/tests/TaskSmackTests`) without setting `LD_LIBRARY_PATH`,
> the GPU mock tests will be skipped automatically via `GTEST_SKIP()`.

## Sanitizers (Linux only)

AddressSanitizer + UndefinedBehaviorSanitizer:

```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

`ctest --preset asan-ubsan` injects:

- `LSAN_OPTIONS=suppressions=${sourceDir}/tests/sanitizer-suppressions/lsan.supp:$penv{LSAN_OPTIONS}`

The project suppression comes first; any caller-provided `LSAN_OPTIONS` are
appended after it, so caller options take precedence. Caller-provided flags such
as verbosity or `halt_on_error` are preserved. Note that `suppressions=` is a
scalar key — if the caller already provides a `suppressions=` entry, it overrides
the project's; in that case add required entries to `lsan.supp` directly.

This suppression filters a known SDL3 LeakSanitizer false positive that affects
Core window/application tests.

ThreadSanitizer:

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

`ctest --preset tsan` injects:

- `TSAN_OPTIONS=suppressions=${sourceDir}/tests/sanitizer-suppressions/tsan.supp:$penv{TSAN_OPTIONS}`

The project suppression comes first; caller-provided `TSAN_OPTIONS` are appended
after it and take precedence. Because `suppressions=` is a scalar key, a
caller-provided `suppressions=` will override the project's — add entries to
`tsan.supp` directly in that case.

This suppression filters known ThreadSanitizer false positives in third-party
pthread barrier paths, while preserving any caller-provided TSAN flags.

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

For repeatable benchmarking runs (recommended), use the helper scripts:

```bash
# Linux
./tools/bench.sh benchmark

# Windows
pwsh tools/bench.ps1 win-benchmark
```

### Benchmark Output

By default, benchmarks output to console. You can also:

```bash
# Keep baselines platform-specific for apples-to-apples comparisons
# Linux baseline:    perf-data/linux-baseline.json
# Windows baseline:  perf-data/win-baseline.json

# Compare Windows runs
python -m google_benchmark.compare perf-data/win-baseline.json perf-data/win-benchmark-<timestamp>.json

# Compare Linux runs
python -m google_benchmark.compare perf-data/linux-baseline.json perf-data/benchmark-<timestamp>.json
```

### Available Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_History_*` | Ring buffer operations (push, access, copyTo) |
| `BM_History_MemoryFootprint` | Memory usage tracking for history buffers |
| `BM_ProcessModel_*` | Process enumeration and snapshot computation |
| `BM_ProcessModel_MemoryGrowth` | Memory growth over repeated refresh cycles |
| `BM_ProcessProbe_Enumerate` | Raw OS API performance |
| `BM_SystemModel_*` | System metric sampling and history accessor performance |
| `BM_SystemModel_MemoryGrowth` | Memory growth over repeated `refresh()` calls exercising the full probe read, delta computation, and history append path |
| `BM_SystemProbe_Sample` | Raw OS system probe API performance |
| `BM_Format_*` | UI formatting functions |
| `BM_NetlinkSocketStats_*` | Netlink INET_DIAG socket query performance (Linux only) |
| `BM_StorageModel_*` | Storage probe/model sampling, history accessor, and per-disk snapshot performance |
| `BM_StorageModel_MemoryGrowth` | Memory growth over repeated `sample()` cycles |
| `BM_GPUModel_*` | GPU probe enumeration, counter reads, model refresh, and history accessor performance |
| `BM_GPUModel_MemoryGrowth` | Memory growth over repeated GPU `refresh()` cycles |
| `BM_Numeric_*` | Micro-benchmarks for `toDouble`, `clampPercentToFloat`, `narrowOr`, and mixed process-table workload |

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

## Profile-Guided Optimization (PGO)

PGO uses real runtime behavior to guide the compiler's optimization decisions — inlining, branch prediction hints, layout — resulting in measurable throughput gains (typically 5–15% on hot paths). TaskSmack uses Clang's instrumentation-based PGO.

### How it works

1. **Build an instrumented binary** (`pgo-generate`/`win-pgo-generate` preset) with `-fprofile-instr-generate`
2. **Run the binary** (benchmarks and/or the app itself) to collect branch-count data into `.profraw` files
3. **Merge** the `.profraw` files into a single `.profdata` with `llvm-profdata`
4. **Build the optimized binary** (`pgo-use`/`win-pgo-use` preset) with `-fprofile-instr-use=<path>.profdata`

### Automated workflow (recommended)

Use the provided helper scripts to run all three phases:

```bash
# Linux – full workflow (build instrumented, run benchmarks, merge, build optimized)
./tools/pgo.sh

# Run individual phases
./tools/pgo.sh generate   # Phase 1: instrumented build + profile collection
./tools/pgo.sh merge      # Phase 2: merge *.profraw → profiles/tasksmack.profdata
./tools/pgo.sh use        # Phase 3: build PGO-optimized binary

# Optimized binary ends up at:
build/pgo-use/bin/TaskSmack
```

```powershell
# Windows – full workflow
# Requires LLVM 22: set LLVM_ROOT to your LLVM 22 install directory
# (e.g. C:\Program Files\LLVM) before running.
pwsh tools/pgo.ps1

# Individual phases
pwsh tools/pgo.ps1 generate
pwsh tools/pgo.ps1 merge
pwsh tools/pgo.ps1 use

# Optimized binary ends up at:
build\win-pgo-use\bin\TaskSmack.exe
```

### Manual workflow

```bash
# Phase 1 – instrumented build
cmake --preset pgo-generate
cmake --build --preset pgo-generate

# Phase 1 (continued) – collect profile data
# %p in LLVM_PROFILE_FILE expands to the PID, preventing clobbering during parallel runs
mkdir -p profiles
LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
    ./build/pgo-generate/bin/TaskSmackBenchmarks --benchmark_min_time=0.5

# Optionally run the app too (more representative sample of UI paths)
LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
    ./build/pgo-generate/bin/TaskSmack
# (exit after a few seconds of normal use)

# Phase 2 – merge profraw files
# Use llvm-profdata from your LLVM 22 install (llvm-profdata-22 on Debian/Ubuntu,
# or llvm-profdata if LLVM 22 is the default on PATH). tools/pgo.sh does this automatically.
LLVM_PROFDATA_BIN="$(command -v llvm-profdata-22 || command -v llvm-profdata)"
"$LLVM_PROFDATA_BIN" merge -sparse profiles/*.profraw -o profiles/tasksmack.profdata

# Phase 3 – PGO-optimized build (reads profiles/tasksmack.profdata)
cmake --preset pgo-use
cmake --build --preset pgo-use
```

### Profile data files

The `profiles/` directory stores collected `.profraw` and merged `.profdata` files:

- `profiles/*.profraw` – per-run raw profile data (auto-cleaned by `pgo.sh generate`)
- `profiles/tasksmack.profdata` – merged profile data consumed by the `pgo-use` preset

These files are `.gitignore`-d and should not be committed. Re-generate them whenever significant code changes are made to keep the profile representative.

### Tips

- **Run real workloads, not just benchmarks.** The benchmarks cover hot paths well, but briefly running the app with a few hundred processes visible gives the compiler more signal for UI and rendering code.
- **Re-profile after large refactors.** Stale profile data still improves performance, but fresh data gives the best results.
- **Combine with the `optimized` preset flags.** The `pgo-use` preset already includes `-O3 -march=x86-64-v3` for maximum effect.
- **Verify end-to-end improvement.** Comparing `benchmark` to `pgo-use` measures the combined effect of PGO and the extra `-march=x86-64-v3` tuning enabled by `pgo-use`, not PGO in isolation. To isolate pure PGO gains, use a baseline build with the same non-PGO flags as `pgo-use`.

```bash
# Baseline (generic optimized benchmark build; no PGO)
cmake --preset benchmark && cmake --build --preset benchmark
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_format=json > /tmp/baseline.json

# PGO + architecture-tuned build (after running tools/pgo.sh)
./build/pgo-use/bin/TaskSmackBenchmarks --benchmark_format=json > /tmp/pgo.json

# Compare (requires: pip install google-benchmark)
python -m google_benchmark.compare /tmp/baseline.json /tmp/pgo.json
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

GitHub Actions runs:

- `ci.yml` (fast path for push/PR):
  - Build + tests (debug and release) on Linux and Windows via reusable matrix workflow
  - clang-tidy (`static-analysis` job)
  - Markdown link audit (`docs-hygiene` job)
  - Include analysis (`include-analysis` job — manual-only via workflow_dispatch)
- `heavy-checks.yml` (heavy path):
  - Weekly on `main` + manual dispatch
  - Coverage (`coverage` job)
  - Sanitizers (Linux: ASan+UBSan, TSan)
- `pre-commit.yml`:
  - pre-commit hooks (includes formatting and hygiene checks)
- `osv-scanner.yml`:
  - Dependency vulnerability scan (Syft SBOM + OSV database)

PR optimization: docs-only pull requests skip compile/test and environment-validation jobs in `ci.yml` to keep feedback fast.

Dependabot updates GitHub Actions and Python dependencies weekly.
[OSV Scanner](https://google.github.io/osv-scanner/) scans C++ FetchContent dependencies
(via Syft SBOM generated from `CMakeLists.txt`) and Python `requirements.txt` against the
[OSV vulnerability database](https://osv.dev) on pushes to `main`, pull requests targeting
`main`, and the weekly scheduled run. Results appear in the repository's **Security → Code scanning** tab.

### Release Artifacts

Each GitHub release (triggered by a `v*.*.*` tag) includes:

- Linux packages: `.tar.gz` and `.deb`
- Windows packages: `.zip`
- SBOM: `tasksmack-<label>-sbom.spdx.json` (where `<label>` is the release tag with any non-`[a-zA-Z0-9._-]` characters replaced by `-`) — an SPDX-JSON Software Bill of Materials generated from the source tree using [Syft](https://github.com/anchore/syft) via [`anchore/sbom-action`](https://github.com/anchore/sbom-action). The SBOM lists all detected components and licenses to improve supply-chain transparency.

### Changelog

[CHANGELOG.md](CHANGELOG.md) is auto-generated by [git-cliff](https://git-cliff.org/) on every `v*.*.*` tag push via the `changelog` workflow. It follows the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format and groups entries by conventional commit type (`feat` → Added, `fix` → Fixed, etc.).

**Do not edit CHANGELOG.md manually.** The `changelog` workflow regenerates the file and opens a pull request that must be merged; it does not push directly to `main`.

#### Producing a Changelog

Producing a changelog is a single command:

```bash
# 1. Make sure you are on main and fully up to date
git checkout main
git pull

# 2. Create and push a strict semver tag — this is the only trigger
git tag v1.0.0
git push origin v1.0.0
```

The `changelog` workflow then runs automatically and:

1. Validates the tag is strict semver (`vMAJOR.MINOR.PATCH`). Tags like `v1.0.0-rc1` or `v1.0.0.4` are rejected.
2. Verifies the tag points to the current `HEAD` of `main`. Tagging from a branch or an older commit will abort the workflow.
3. Runs git-cliff over the full commit history to generate `CHANGELOG.md`.
4. Opens a pull request (`chore/changelog-vX.Y.Z → main`) with the updated `CHANGELOG.md`.
5. Merge the PR once CI passes — it will be labeled `documentation` for easy filtering.

You can watch the progress under **Actions → Changelog** in the GitHub UI.

#### Writing Commits for Meaningful Changelog Entries

The output quality depends entirely on commit messages. Follow [Conventional Commits](https://www.conventionalcommits.org/):

```text
feat: add network panel
fix: handle zero-division in CPU delta calculation
perf: reduce snapshot allocation in hot path
security: sanitize process name input
docs: update architecture overview
test: add ProcessModel delta edge cases
ci: cache FetchContent dependencies
build: bump minimum CMake to 3.28
chore: remove unused include
revert: revert "feat: add network panel"
```

These prefixes map to changelog sections as follows:

| Commit prefix | Changelog section |
|---|---|
| `feat:` | Added |
| `fix:` | Fixed |
| `security:` | Security |
| `perf:`, `refactor:`, `style:` | Changed |
| `docs:` | Documentation |
| `test:`, `ci:`, `build:` | Infrastructure |
| `chore:` | Maintenance |
| `revert:` | Reverted |
| `Merge ...` | _(skipped — noise)_ |
| Unconventional messages | _(skipped)_ |

Issue and PR references like `(fixes #42)` or `(#42)` in commit messages are automatically hyperlinked to GitHub.

Breaking changes are highlighted when you add a `!` after the prefix or include a `BREAKING CHANGE:` footer:

```text
feat!: remove legacy probe interface
```

The git-cliff configuration lives in [`cliff.toml`](cliff.toml) at the repo root.

### CI Artifacts (GitHub UI)

In Actions → workflow run → Artifacts, you may see:

- `coverage-report`
- `asan-ubsan-report`
- `tsan-report`
- `linux-debug-test-results`, `linux-release-test-results`
- `windows-debug-test-results`, `windows-release-test-results`
- `clang-tidy-results`
- `iwyu-results`

### CI Artifacts (GitHub CLI)

```bash
gh run download <run-id> -n coverage-report
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

## Code Review Expectations

### For authors
- Keep PRs small and focused (< 200 lines changed is ideal; see the PR template for guidelines).
- Fill out the PR template fully — description, type of change, and testing steps.
- Respond to review comments within a few days; mark threads resolved after addressing them.
- Do not force-push after a review has started unless asked to rebase.

### For reviewers
- Aim to complete reviews within 2–3 business days.
- Distinguish blocking concerns (must fix) from suggestions (nice to have) in comments.
- Approve once all blocking issues are addressed; don't hold approval for minor nits.
- When evaluating layer boundaries, refer to the architecture section in [.github/copilot-instructions.md](.github/copilot-instructions.md).

### Merge criteria
- All CI checks pass.
- At least one approving review.
- No unresolved blocking comments.

## Reporting Issues

Please use the issue templates:

- Bug Report: [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)
- Feature Request: [.github/ISSUE_TEMPLATE/feature_request.md](.github/ISSUE_TEMPLATE/feature_request.md)

## Security Issues

See [SECURITY.md](SECURITY.md) for responsible disclosure. Do not open public issues for security vulnerabilities.
