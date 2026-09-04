# Contributing to TaskSmack

Thanks for contributing!

This document is the single source of truth for developer setup and workflows (build, test, format, lint, profiling, and packaging).

## Documentation

To avoid duplication and doc drift, these are the canonical docs:

- [README.md](README.md): project landing page and documentation index
- [CONTRIBUTING.md](CONTRIBUTING.md): contributor workflow (this file)
- [tasksmack.md](tasksmack.md): architecture, metrics pipeline, and engineering direction
- [completed-features.md](completed-features.md): canonical implemented-feature inventory
- [docs/guide/](docs/guide/): user guide and troubleshooting
- [docs/dev/](docs/dev/): concise docs-site navigation back to these canonical developer sources
- [.github/copilot-instructions.md](.github/copilot-instructions.md) and [.github/copilot-coding-agent-tips.md](.github/copilot-coding-agent-tips.md): agent guidance (also useful to contributors)

Avoid copying contributor commands or architecture diagrams into additional Markdown files. Link to the relevant canonical section instead.

### C++ API Documentation

JSDoc and language-level docstrings are not C++ conventions. When declaration-level API documentation is useful, use Doxygen-compatible `///` or `/** ... */` comments in headers. Document contracts, ownership, units, thread safety, and platform limitations; do not narrate self-explanatory accessors or repeat implementation details. Repository-level design and workflow guidance belongs in the Markdown sources above.

## Quick Start

```bash
git clone https://github.com/mgradwohl/tasksmack.git
cd tasksmack

# Python 3.14+ is required.
python3.14 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt

# Set up pre-commit hooks (recommended)
pre-commit install
```

### Automated Setup (Linux)

Instead of installing prerequisites manually, run:

```bash
./tools/setup-dev.sh          # Install all prerequisites automatically
source .venv/bin/activate     # Use the project-local Python environment
./tools/check-prereqs.sh      # Verify environment after setup
```

The setup script supports Ubuntu and creates `.venv/` for Python 3.14 tooling without changing the system `python` commands. It installs GLAD's hash-locked build dependencies and, unless `--minimal` is used, the development dependencies from `requirements.txt`. Use `--dry-run` to preview what will be installed without making changes, or `--minimal` to install only build tools (skipping coverage/profiling).

### Automated Setup (Windows)

```powershell
pwsh tools/setup-dev.ps1      # Install all prerequisites via winget
pwsh tools/check-prereqs.ps1  # Verify environment
```

The Windows setup installs the Visual Studio 2022 C++ Build Tools workload (including a compatible Windows SDK), the requested LLVM version, CMake, Ninja, Python 3.14, ccache, GLAD's hash-locked build dependencies, and, unless `-Minimal` is used, the development dependencies from `requirements.txt`.

### One-Command Dev Workflow

After setup, use CMake workflow presets for the full configure → build → test cycle in a single command:

```bash
cmake --workflow --preset dev          # Linux debug build + test
cmake --workflow --preset win-dev      # Windows debug build + test
cmake --workflow --preset coverage     # Coverage build + test
cmake --workflow --preset asan-ubsan-cycle # ASan+UBSan
cmake --workflow --preset tsan-cycle       # TSan
```

## Check Prerequisites

If you just want a quick check of your environment, run:

```bash
./tools/check-prereqs.sh    # Linux
.\tools\check-prereqs.ps1   # Windows
```

```bash
# Manual Windows configure, build, and test
cmake --preset win-debug
cmake --build --preset win-debug
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
- Python 3.14+ with jinja2 (required for GLAD OpenGL loader generation)
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
- Python 3.14+ with jinja2 (required for GLAD OpenGL loader generation)
- FreeType 2.13+ (font rendering library) - typically auto-detected or fetched if not found
- **Windows SDK** — required (Clang needs its headers/libraries to compile Windows C++ code at all, regardless of which RC compiler is used); installed automatically by Visual Studio 2022 C++ Build Tools (`tools/setup-dev.ps1`); see below for how the RC (resource) compiler itself is selected

**RC (resource) compiler**: the build compiles `assets/tasksmack.rc.in` (icon and version
info -- the DPI-awareness manifest is separate, embedded via `/MANIFESTINPUT` at link
time, so it doesn't depend on RC compilation at all) into `TaskSmack.exe`.
`CMakeLists.txt` (see `cmake/RCCompiler.cmake`) requires `llvm-rc` (installed alongside
`clang`/`clang++`, so no setup beyond the LLVM install above is needed) instead of the
Windows SDK's own `rc.exe`, and fails configuration outright with an actionable error if
`llvm-rc` can't be found -- there is no silent fallback to `rc.exe`, since that's the
exact RC compiler known to hang indefinitely on this project (#746). **You no longer
need to run from inside a Visual Studio Developer Command Prompt** (`vcvarsall.bat`)
just to get a working RC compiler, unlike some other Clang+MSVC-resource-compiler
setups. To use a different RC compiler instead, opt in explicitly by passing
`-DCMAKE_RC_COMPILER=<path>` to `cmake --preset ...` or setting the `RC` environment
variable before configuring, both of which skip auto-detection -- **with one exception:
use `RC` specifically to pin `rc.exe`**, not `-DCMAKE_RC_COMPILER=`. On a build tree
that predates this fix (or on the very first configure of a brand-new one),
`CMakeLists.txt` can't yet tell an intentional `-DCMAKE_RC_COMPILER=<path to rc.exe>`
override apart from a stale cached value from before, and -- since `rc.exe` is the one
tool with a confirmed hang -- resolves that specific ambiguity in favor of safety by
re-detecting `llvm-rc` instead, ignoring the override. `RC` isn't affected by this at
all and always wins. Any other RC compiler passed via `-DCMAKE_RC_COMPILER=` (i.e. not
named `rc.exe`) doesn't hit this ambiguity and is always honored as pinned. Also like
CMake's `CC`/`CXX`, `RC` may include compiler arguments (e.g. `RC="C:\tools\llvm-rc.exe
--flag"`) -- these are split out automatically -- and, following that same CC/CXX
convention, a path containing a space (e.g. the default `C:\Program Files\LLVM\bin`)
must be quoted within the value itself: `RC="\"C:\Program Files\LLVM\bin\llvm-rc.exe\""`.

Install Python + jinja2:

```powershell
winget install Python.Python.3.14
py -3.14 -m pip install --require-hashes -r requirements-glad.lock
```

## Pre-commit Hooks (Recommended)

Pre-commit hooks automatically check your code before each commit, catching formatting and style issues early. This is **strongly recommended** to avoid CI failures.

### Install

```bash
# Activate the project environment created during setup, then install pre-commit.
source .venv/bin/activate
python -m pip install pre-commit

# Install the git hooks (run from project root)
pre-commit install
```

Or install from requirements.txt:

```bash
python -m pip install -r requirements.txt
pre-commit install
```

`requirements.txt` is a hash-locked file generated from `requirements.in`. To change or upgrade Python dependencies, edit `requirements.in` and regenerate:

```bash
python -m pip install pip-tools==7.6.1
pip-compile --generate-hashes --output-file=requirements.txt requirements.in
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

### Cleaning Build Artifacts

Remove stale build directories, FetchContent cache, and coverage output:

```bash
./tools/clean.sh          # Remove build/ and coverage/  (Linux)
./tools/clean.sh --all    # Also removes .cache/, dist/, profiles/, and compilation databases
./tools/clean.sh --dry-run  # Preview what would be removed without deleting anything

pwsh tools/clean.ps1      # Windows equivalent (same flags)
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
| `optimized` | `win-optimized` | LTO, march=x86-64-v3, stripped, whole-program vtables (Haswell 2013+) |
| `coverage` | `win-coverage` | Debug + code coverage instrumentation |
| `asan-ubsan` | — | AddressSanitizer + UBSan (Linux only) |
| `tsan` | — | ThreadSanitizer (Linux only) |
| `msan` | — | MemorySanitizer: catches uninitialised-memory reads (Linux/Clang only). **Not CI-verified** — no workflow exercises this preset, and it requires an MSan-instrumented libc++ you build and link against yourself; without one it will report false positives from uninstrumented STL code. |
| `unity` | `win-unity` | Unity (jumbo) build for fast end-to-end checks; trades incremental correctness for speed |

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

For `Platform::IProcessActions` (process kill/terminate/stop/resume/setPriority), use
`TestMocks::MockProcessActions` in the same header: it lets each action's result be configured
independently (`setKillResult(...)`, etc.) and tracks the last pid (and, for `setPriority`, the
nice value) and call count per method, so a test can assert both what was called and with what
argument.

### Testing App/UI code that needs a live ImGui context

`TaskSmackTests` does not link the real ImGui/ImPlot library object code (no `imgui`/`implot`
library target, no live window or GL context) - only their headers are on the include path. This
means a `.cpp` file that calls real `ImGui::`/`ImPlot::`/`ImGui_Impl*::` functions (window setup,
widget rendering, font atlas baking, etc.) cannot be added to `TaskSmackTests`' source list: it
will fail to *link*, not just to run correctly, with undefined references to those symbols. This
is why `TitleBarLayer.cpp`, `ProcessesPanel.cpp`, `ProcessDetailsPanel.cpp`, `SystemMetricsPanel.cpp`,
and `UILayer.cpp` are not linked into `TaskSmackTests` even though their headers are tested.

Two established ways to get real coverage of such a file's logic anyway:

1. **Extract the pure decision logic into a small header**, taking every input as an explicit
   parameter instead of reading member/global state, and `#include` it back into the original
   `.cpp`. The original file keeps a thin wrapper that supplies the live inputs (mouse position,
   SDL window state, etc.); the header holds the actual decision and gets tested directly. See
   `App/TitleBarGeometry.h` (`computeWindowHitTest`, `computeDetectResizeEdge`,
   `computeIsPointInBounds`, ...), `App/Panels/ProcessDetailsPanel_ActionHelpers.h`
   (`dispatchProcessAction`), and `UI/DpiScale.h`/`UI/MonospaceFontPath.h` for the pattern. When
   the extracted logic needs to probe the filesystem or environment, take an injectable predicate
   (`std::function<bool(const std::filesystem::path&)>` etc.) the same way `UI::selectAssetsDir()`
   (`AssetPath.h`) already does, rather than hard-coding a real filesystem/env call into the
   testable function.
2. **Link the real file directly, if it turns out not to need the ImGui *library* at all.** Not
   every file that `#include`s `imgui.h` actually calls into ImGui - some only use its type
   declarations (e.g. `ImTextureID`, `ImVec2`) for their own return types. `UI/IconLoader.cpp` is
   linked into `TaskSmackTests` (see `tests/CMakeLists.txt`) for exactly this reason: it only
   needs `imgui.h` for types, and its OpenGL calls resolve against `glad_gl_core_33`, which *is*
   already linked. Before reaching for extraction, check whether the file actually calls any
   `ImGui::`/`ImPlot::` function - if it doesn't, linking it directly gives real coverage of the
   actual production code instead of a parallel copy.

Before writing either kind of test, check whether the logic already exists as a shared, tested
helper - `TitleBarLayer.cpp` used to have its own private case-insensitive env-flag parser that
turned out to be a byte-for-byte duplicate of the already-shared, already-tested
`Core::isEnvFlagEnabled()` (`Core/EnvUtils.h`); deleting the duplicate and reusing the shared
helper was strictly better than writing a third copy of the same test.

## VS Code

Recommended extensions:

- clangd (LLVM)
- CodeLLDB
- CMake Tools

Workspace settings resolve `clangd` and `clang-format` from `PATH` so the same `.vscode/settings.json` works across Linux/WSL/Windows. Ensure LLVM tools are on `PATH` (the prerequisite scripts validate this). Only use user-level VS Code overrides if your local install path is nonstandard.

Personal preferences - custom terminal profiles (for example a profile that sources your own rcfile), default terminal selection, and chat/agent tool auto-approval - belong in your user-level `settings.json` (`Preferences: Open User Settings (JSON)`), not in the repo's workspace settings.

Before troubleshooting VS Code diagnostics, run the prerequisite check to confirm required tools are installed and discoverable on `PATH`:

```bash
./tools/check-prereqs.sh      # Linux
pwsh tools/check-prereqs.ps1  # Windows
```

If this check passes, `clangd`/`clang-format` should be auto-discovered by the workspace settings.

clangd reads `compile_commands.json` from the repository root. The `debug` and `win-debug` presets copy it there automatically after each build via the opt-in `TASKSMACK_COPY_COMPILE_COMMANDS` CMake option; other presets leave the source tree untouched. To get the copy behavior with a different preset, configure with `-DTASKSMACK_COPY_COMPILE_COMMANDS=ON`.

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
./tools/coverage.sh --preset coverage   # optional; defaults to "coverage"
# Windows
pwsh tools/coverage.ps1
pwsh tools/coverage.ps1 -OpenReport
pwsh tools/coverage.ps1 -Preset win-coverage   # optional; defaults to "win-coverage"
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

## Fuzzing (Linux only)

ClusterFuzzLite continuously exercises the allocation-free `/proc` numeric
parsers with libFuzzer and AddressSanitizer. Pull requests that change the
parser or fuzzing configuration run a short code-change fuzzing job; `main`
also produces a baseline build. Separate weekly jobs perform a longer batch
run and prune the resulting corpus.

To run the current target locally with Clang:

```bash
mkdir -p build/fuzz
clang++-22 -std=c++23 -Isrc -fsanitize=fuzzer,address \
  tests/fuzz/fuzz_proc_parsing.cpp -o build/fuzz/fuzz_proc_parsing
./build/fuzz/fuzz_proc_parsing -max_total_time=60
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

The `profile` and `win-profile` presets build at `-O2 -g -DNDEBUG` with frame pointers
preserved (`-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`). All profiling scripts
write artifacts under `perf-data/` and emit `KEY=value` lines at exit for scripting.

### Linux — CPU profiling (perf)

Use `tools/profile-perf.sh` to capture and `tools/analyze-perf.sh` to analyze.
Default preset is `profile` for app mode and `benchmark` for bench mode.

```bash
# App trace — exercise the app, then close it
./tools/profile-perf.sh app

# Benchmark trace — targeted hot-path capture
./tools/profile-perf.sh bench
./tools/profile-perf.sh bench --bench-filter 'BM_ProcessModel_Refresh$'

# Analyze a captured trace (top functions + optional flamegraph SVG)
./tools/analyze-perf.sh perf-data/perf-app-<timestamp>.data

# Open in Hotspot GUI (if installed: sudo apt install hotspot)
hotspot perf-data/perf-app-<timestamp>.data

# Quick perf stat counters (no trace file)
perf stat ./build/profile/bin/TaskSmackBenchmarks --benchmark_filter=BM_ProcessModel_Refresh
```

**Flamegraph generation** requires the Brendan Gregg FlameGraph scripts:
```bash
git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph
export PATH="$HOME/opt/FlameGraph:$PATH"
# analyze-perf.sh auto-detects them and generates the SVG
./tools/analyze-perf.sh perf-data/perf-app-<timestamp>.data
```

**WSL2 note:** `perf` requires a kernel-matched tools package. If you see a version
mismatch warning, run `sudo apt install linux-tools-$(uname -r) linux-tools-generic`
or profile on a native Linux machine / GitHub Actions runner.

### Linux — Heap allocation profiling (heaptrack)

Use `tools/profile-heap.sh` to find hot-path heap allocations that don't show up
in CPU profiles. Approximately 2–3× runtime overhead (vs. Valgrind's ~50×).

```bash
# Install
sudo apt install heaptrack

# App trace
./tools/profile-heap.sh app

# Benchmark trace — pinpoint per-call allocation sources
./tools/profile-heap.sh bench
./tools/profile-heap.sh bench --bench-filter 'BM_ProcessModel_Refresh$'

# Open in GUI
heaptrack_gui perf-data/heaptrack-app-<timestamp>.gz

# Headless analysis (CI-friendly)
heaptrack_print perf-data/heaptrack-app-<timestamp>.gz
```

### Windows — CPU profiling (ETW)

Use `tools/profile-etw.ps1` to capture and `tools/analyze-etw.ps1` to analyze.
ETW capture self-elevates; the scripts build before prompting for elevation by default.

```powershell
# App trace — exercise the app, then close it (defaults to win-optimized)
pwsh tools/profile-etw.ps1 app

# Benchmark trace
pwsh tools/profile-etw.ps1 bench
pwsh tools/profile-etw.ps1 bench -BenchmarkFilter 'BM_ProcessModel_Refresh$'

# Use win-profile for symbol-rich follow-up attribution
pwsh tools/profile-etw.ps1 app -Preset win-profile

# Analyze a captured trace
pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-<timestamp>.etl

# Skip function decoding (faster, module-level only)
pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-<timestamp>.etl -SkipFunctions

# Optional VTune workflow if installed
vtune -collect hotspots -- .\build\win-profile\bin\TaskSmack.exe
```

Notes:
- `wpr`, `xperf`, and `wpa` ship with the Windows Performance Toolkit (install via Windows SDK).
- ETW capture requires elevation; `profile-etw.ps1` relaunches itself as Administrator automatically and validates all output artifacts before returning.
- `wpr -cancel` returns a non-zero exit code when no trace is active; the scripts treat that as non-fatal.
- Prefer `win-optimized` for real-world timing; use `win-profile` when you need function-level symbol attribution.
- Function decoding against `win-optimized` binaries may be limited (no debug info); `analyze-etw.ps1` degrades gracefully with an explanatory message.

### Compile-Time Profiling (-ftime-trace)

To identify slow headers and compilation bottlenecks, use the `TASKSMACK_ENABLE_TIME_TRACE` CMake option:

```bash
# Configure with -ftime-trace enabled (Clang emits per-TU .json trace files)
cmake --preset debug -DTASKSMACK_ENABLE_TIME_TRACE=ON
cmake --build --preset debug

# Collect and merge all trace files, then open in chrome://tracing or Perfetto
./tools/time-trace.sh debug

# Or list trace files without opening
./tools/time-trace.sh --list
```

Each `.cpp` file generates a `<source>.json` trace alongside its `.o` file. `tools/time-trace.sh` merges all traces into `build/debug/time-trace-merged.json` for easy visualization.

### Resize Performance Instrumentation

TaskSmack has built-in resize/interaction frame-timing instrumentation that works in any build.
Enable it by setting `TASKSMACK_TRACE_RESIZE_PERF=1` before launching:

```bash
# Linux — optimized build (recommended for realistic numbers)
cmake --preset optimized
cmake --build --preset optimized
TASKSMACK_TRACE_RESIZE_PERF=1 ./build/optimized/bin/TaskSmack 2>&1 | tee /tmp/resize-trace.log

# Linux — profile build (frame pointers preserved for follow-up perf/flamegraph)
cmake --preset profile
cmake --build --preset profile
TASKSMACK_TRACE_RESIZE_PERF=1 ./build/profile/bin/TaskSmack 2>&1 | tee /tmp/resize-trace.log
```

```powershell
# Windows — profile build
cmake --preset win-profile
cmake --build --preset win-profile
$env:TASKSMACK_TRACE_RESIZE_PERF=1; .\build\win-profile\bin\TaskSmack.exe 2>&1 | Tee-Object /tmp/resize-trace.log
```

Resize the window (edges and corners) for 20–30 seconds, then close the app.
The log contains `ResizePerf[interaction-progress|interaction-end|shutdown]` lines with
per-phase timing for every 0.5 s window:

```
ResizePerf[interaction-progress]: batches=109 events=48 resizeEvents=36 maxBatchEvents=4
  frames=109 resizeFrames=109
  drain avg/max=0.140/2.278 ms    ← SDL event drain
  update avg/max=0.018/1.453 ms   ← domain model refresh (all layers)
  render avg/max=0.572/8.798 ms   ← ImGui layout + draw call generation
  post avg/max=0.558/0.784 ms     ← post-render (all layers)
  swap avg/max=3.299/12.408 ms    ← GL buffer swap (includes vsync stall)
```

Frames or layers that exceed 250 ms emit additional `ResizePerfSlowFrame` /
`ResizePerfSlowLayer` / `ResizePerfTitleBarSlowUpdate` lines for pinpoint attribution.

You can also control the spdlog runtime level directly (useful for CI or scripted runs):

```bash
# Show only info+ in a release build (same effect as TASKSMACK_TRACE_RESIZE_PERF=1)
TASKSMACK_LOG_LEVEL=info ./build/optimized/bin/TaskSmack

# Full debug verbosity in an optimized build
TASKSMACK_LOG_LEVEL=debug ./build/optimized/bin/TaskSmack
```

`TASKSMACK_LOG_LEVEL` accepts any spdlog level name: `trace`, `debug`, `info`, `warn`,
`error`, `critical`, `off`. When both env vars are set, `TASKSMACK_LOG_LEVEL` takes
precedence.

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
| `TASKSMACK_ENABLE_TIME_TRACE` | `OFF` | Enable `-ftime-trace` for TaskSmack sources (Clang): per-TU compile-time flamegraphs |
| `TASKSMACK_ENABLE_UNITY_BUILD` | `OFF` | Enable unity compilation for TaskSmack-owned targets |
| `TASKSMACK_LINKER` | `lld` | Linker used by all TaskSmack executables: `lld`, `mold`, or `default` |

To disable warnings-as-errors for local iteration:

```bash
cmake --preset debug -DTASKSMACK_WARNINGS_AS_ERRORS=OFF
```

## Build System Notes

Some choices are intentional (to keep the build predictable across Windows/Linux):

- CMake Presets are the source of truth for configurations
- FetchContent is used for dependencies (prefer `SYSTEM` to reduce third-party warning noise)
- Presets use libc++ on Linux and the MSVC STL on Windows

The root `CMakeLists.txt` is intentionally declarative and delegates to focused modules under `cmake/` (include order matters — options first, then compiler setup, then dependencies):

| Module | Responsibility |
| --- | --- |
| `cmake/Options.cmake` | All `option()`/cache variable declarations (`TASKSMACK_*`) |
| `cmake/CompilerOptions.cmake` | Language standards, warnings, hardening, ccache/sccache, IPO, linker selection, `tasksmack_apply_default_warnings()`, `tasksmack_apply_linux_toolchain()` |
| `cmake/Dependencies.cmake` | FetchContent cache + all third-party dependencies and their target wiring (`imgui_lib`, `implot_lib`, GLAD, etc.) |
| `cmake/StaticAnalysis.cmake` | clang-tidy/clang-format discovery, stripped clang-tidy compile database, `run-clang-tidy`, `run-clang-format`, `copy-compile-commands` targets |
| `cmake/PrecompiledHeaders.cmake` | PCH header list for the app target |
| `cmake/TestPrecompiledHeaders.cmake` | PCH header list for the test target |
| `cmake/InstallRules.cmake` | `install()` rules for binaries, libs, and assets |
| `cmake/Packaging.cmake` | CPack configuration (ZIP/TGZ/NSIS/DEB/RPM) |

The root file keeps project setup, the version header, source/header lists, the `TaskSmack` target definition, and `tests`/`benchmarks` subdirectory wiring. Note that `CompilerOptions.cmake` must stay included before `Dependencies.cmake` so global flags (coverage, hardening, compiler launcher) apply to third-party builds, and `StaticAnalysis.cmake` is included before the Windows `.rc` file is appended to `TASKSMACK_SOURCES` so analysis targets only see real C++ sources.

Clang-tidy configuration is curated for signal/noise; see `.clang-tidy` for the current list of disabled checks. Work to re-enable selected checks is tracked in GitHub issues; #60, #61, #62, and #64 are done, and #63 (`modernize-use-auto`) is in progress (PR #738).

## Adding Dependencies

Use CMake’s `FetchContent` for dependencies. Declare new dependencies in `cmake/Dependencies.cmake`, and always use `SYSTEM` to suppress third-party warnings:

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

We use GitHub Actions for our CI workflows. They are categorized as follows:

### Core Build & Test
- **`ci.yml`**: The primary hub. Runs on pushes to `main`/`dev/**`, all PRs, weekly, and via manual dispatch. It detects docs-only changes to skip C++ builds. It runs Linux and Windows Debug builds on push/PR, Release builds on schedule/dispatch, checks markdown links, runs IWYU (include analysis) only via manual dispatch, and runs a non-blocking advisory Address/Undefined Behavior sanitizer on PRs. It outputs a `ci-success` gate job used for branch protection.
- **`reusable-build-test.yml`**: Contains the actual matrix steps for setting up LLVM, Python, `ccache`, configuring CMake, building, and running CTest tests. Called by other workflows.
- **`manual-build.yml`**: Manual dispatch entry point to trigger a specific OS and build type build from the GitHub UI without opening a PR.

### Security & Fuzzing
- **`codeql.yml`**: Runs GitHub's CodeQL engine to trace execution and analyze the C/C++ codebase for semantic security vulnerabilities (pushes/PRs to main, weekly).
- **`osv-scanner.yml`**: Uses Google's OSV-Scanner to check dependencies against the Open Source Vulnerability database (pushes to main, weekly, manual dispatch).
- **`scorecard.yml`**: Evaluates the repository against OpenSSF security best practices (branch protection, pinned dependencies) and uploads results to the security dashboard (pushes/weekly).
- **`dependency-review.yml`**: Scans PRs to block any that introduce vulnerable dependencies (CVE-based) in package manifests/lockfiles.
- **`sanitizers.yml`**: Performs heavy blocking runs using Address/Undefined Behavior (ASan+UBSan) and Thread (TSan) sanitizers on pushes to `main`, generating HTML reports of memory leaks or data races.
- **ClusterFuzzLite (`cflite_*.yml`)**: Google's continuous fuzzing suite. Runs on PRs (`cflite_pr.yml`), pushes to main (`cflite_build.yml`), and weekly for batching and pruning corpora (`cflite_batch.yml`, `cflite_prune.yml`).

### Code Quality & Hygiene
- **`pre-commit.yml`**: Runs the `pre-commit` framework (via Python) across all files to enforce syntax hygiene, formatting, and file-level rules configured in `.pre-commit-config.yaml` (pushes to main, PRs).
- **`static-analysis.yml`**: Dedicated workflow for running `clang-tidy` against the codebase (pushes to main, manual dispatch).
- **`heavy-checks.yml`**: Runs expensive verifications that shouldn't block PR feedback loops, such as generating Coverage reports (pushes to main, schedule).

### Release & Operations
- **`release.yml`**: Handles compiling production binaries, packaging them (ZIP/tarballs, deb), and publishing GitHub Releases on `v*.*.*` tags.
- **`changelog.yml`**: Automates changelog generation using `git-cliff` for strict `vMAJOR.MINOR.PATCH` tags.
- **`pr-labeler.yml`**: Automatically assigns labels (e.g., `bug`, `enhancement`, `docs`) to pull requests based on `.github/labeler.yml` file globs.
- **`copilot-setup-steps.yml`**: Bootstraps the repository environment (CMake, LLVM, etc.) for GitHub Copilot cloud agent sessions.

PR optimization: docs-only pull requests skip compile/test and environment-validation jobs in `ci.yml` to keep feedback fast.

Dependabot updates GitHub Actions and Python dependencies weekly.
[OSV Scanner](https://google.github.io/osv-scanner/) scans C++ FetchContent dependencies
(via Syft SBOM generated from `CMakeLists.txt`) and both Python dependency manifests against the
[OSV vulnerability database](https://osv.dev) on pushes to `main` and the weekly scheduled run. Results appear in the repository's **Security → Code scanning** tab.
Release and CI builds install GLAD's Python dependencies from the hash-locked
`requirements-glad.lock`; the LLVM bootstrap action also verifies the downloaded installer's
SHA-256 digest before executing it.

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
