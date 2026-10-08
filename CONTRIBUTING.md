# Contributing to TaskSmack

Thanks for contributing!

This document is the single source of truth for developer setup and workflows (build, test, format, lint, profiling, and packaging). Participation in this project is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).

## Documentation

To avoid duplication and doc drift, these are the canonical docs:

- [README.md](README.md): project landing page and documentation index
- [CONTRIBUTING.md](CONTRIBUTING.md): contributor workflow (this file)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md): contributor conduct policy and reporting path
- [tasksmack.md](tasksmack.md): architecture, metrics pipeline, and engineering direction
- [docs/dev/completed-features.md](docs/dev/completed-features.md): canonical implemented-feature inventory
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

The setup script supports Ubuntu and creates `.venv/` for Python 3.14 tooling without changing the system `python` commands. It installs GLAD's hash-locked build dependencies and, unless `--minimal` is used, the development dependencies from `requirements.txt`, plus the coverage/profiling toolchain: `llvm-cov`/`llvm-profdata`, `heaptrack` (see "Linux — Heap allocation profiling" below), and the FlameGraph scripts (see "Linux — CPU profiling" below), cloned to `~/opt/FlameGraph`. Use `--dry-run` to preview what will be installed without making changes, or `--minimal` to install only build tools (skipping coverage/profiling). `perf` itself is deliberately left as a manual step — it's coupled to the running kernel version, and `check-prereqs.sh` detects and prints the exact install command for it.

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
py -3.14 -m pip install --require-hashes -r requirements-glad.txt
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
python -m pip install --require-hashes -r requirements-dev-tools.txt
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
- **actionlint**: Lint GitHub Actions workflows (expressions, contexts, `needs:`, inputs, and warning-level
  shellcheck findings in `run:` blocks when shellcheck is installed; configured in `.github/actionlint.yaml`)
- **reuse**: [REUSE](https://reuse.software) licensing compliance (`reuse lint`) -- see
  [Licensing (REUSE)](#licensing-reuse)

### Bypassing Hooks (Emergency Only)

If you need to commit without running hooks (not recommended):

```bash
git commit --no-verify
```

## Licensing (REUSE)

The repository follows the [REUSE specification](https://reuse.software/spec/): every file's
copyright and licence is declared, and the `reuse` pre-commit hook (`reuse lint`) fails when one is
missing. Licence texts live in `LICENSES/`; the annotations live in the top-level `REUSE.toml`
rather than in per-file headers.

- **New TaskSmack files** need nothing: the catch-all `path = "**"` block in `REUSE.toml` marks them
  MIT (`2024-2026 Matt Gradwohl`, matching `LICENSE`).
- **New third-party files** (fonts, images, vendored or copied code, patches against upstream code,
  adapted documents) MUST get their own `[[annotations]]` block in `REUSE.toml` with the upstream
  copyright holder and SPDX licence identifier, placed after the catch-all (the last matching block
  wins) with `precedence = "override"`. Keep the upstream notice next to the file too (as
  `assets/fonts/LICENSE.txt` does for the fonts).
- A licence that is not yet in `LICENSES/` must be added there:
  `reuse download <SPDX-ID>` (with `pip install reuse`), or let the hook tell you which one is missing.
- Check locally with `pre-commit run reuse --all-files` (or `reuse lint`).

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
argument. Every action takes a `Platform::ProcessTarget` (PID plus the probe's raw start time),
not a bare PID, and real implementations refuse the action unless the process at that PID has that
start time (#973); `lastTarget()` returns the whole target the most recent action received.

For `Platform::IProcessEnvironmentReader` (the on-demand environment read behind Process Details'
Environment section, #179), use `TestMocks::MockProcessEnvironmentReader`: `setResult(...)` and
`setHasEnvironment(...)` configure it, and `readCount()`/`lastTarget()` let a test check the read
cadence and the target. `Platform::IProcessConnectionsReader` (the Connections section, #799) has
`TestMocks::MockProcessConnectionsReader` on the same terms (`setResult(...)`,
`setHasConnections(...)`, `readCount()`, `lastTarget()`).

### Testing App/UI code that needs a live ImGui context

Shell tab registration and lifecycle forwarding live in the header-only `App/PanelTabs.h`.
`tests/App/test_PanelTabs.cpp` exercises them with fake panels, including selection, dynamic
labels, and content-only rendering, without linking ImGui.

`TaskSmackTests` links the real Dear ImGui and ImPlot libraries (`imgui_lib` and `implot_lib` in
`tests/CMakeLists.txt`, built by `cmake/Dependencies.cmake`; `imgui_lib` also carries the SDL3 and
OpenGL3 backends and FreeType). So a `.cpp` file that calls `ImGui::`/`ImPlot::` functions links
fine and can be driven headless: ImGui and ImPlot need only a context, a display size and a font
atlas to lay out widgets and build draw lists. What the test run does *not* have is a display or a
GL context: CI runs headless, and the `Core` window tests (`tests/Core/test_Window.cpp`) skip when
SDL video or GL is unavailable. Code that needs a real SDL window, an OpenGL context or the
`ImGui_Impl*` platform/renderer backends at run time (`UI/UILayer.cpp`, texture uploads, GL calls)
links but cannot run there.

The production `.cpp` files exercised this way are listed under "Source files under test" in
`tests/CMakeLists.txt` (for example `App/Panels/ProcessActionConfirm.cpp`,
`App/Panels/ProcessActionsView.cpp`, `App/Panels/ProcessPriorityView.cpp`,
`App/Panels/ProcessEnvironmentView.cpp`, `App/Panels/ProcessConnectionsView.cpp`, `App/AboutDialog.cpp`,
`App/HelpWindow.cpp`, `UI/ChartLegend.cpp`).
`TitleBarLayer.cpp`, `ShellLayer.cpp`, `SettingsLayer.cpp`, `AboutLayer.cpp` and `HelpLayer.cpp` (their
windows are `AboutDialog.cpp` and `HelpWindow.cpp`, which are), `ElevationNoticeLayer.cpp`, `ProcessesPanel.cpp`, `ProcessDetailsPanel.cpp`,
`SystemMetricsPanel.cpp`, the `*Section.cpp` tabs and `UI/UILayer.cpp` are not in that list. That
is no longer a link limit on ImGui itself: `UI/Theme.cpp` is replaced in the test binary by
`tests/Mocks/ThemeStub.cpp`, so a file can only be added once every `Theme` member it calls is
stubbed (the stub is a small fake, not a no-op: `setThemeById()` / `setFontSize()` record what they
are given and `currentThemeId()` / `currentFontSize()` return it, so `UserConfig`'s
apply → capture → save → load → apply round trip is tested in `test_UserConfigPersistence.cpp`);
`ProcessesPanel.cpp` and `SystemMetricsPanel.cpp` also create real `Platform` probes
at construction or attach; and `UILayer.cpp` drives the SDL3/OpenGL3 backends against a live
window. Prefer moving a panel's drawing into a small view or helper `.cpp` with explicit inputs
(as `ProcessPriorityView.cpp` and `ProcessActionsView.cpp` were split out of
`ProcessDetailsPanel.cpp`) and linking that.

Three established ways to get real coverage of such a file's logic:

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
2. **Link the real file directly, if it doesn't need a live context at all.** Not every file
   that `#include`s `imgui.h` actually calls into ImGui - some only use its type declarations
   (e.g. `ImTextureID`, `ImVec2`) for their own return types. `UI/IconLoader.cpp` is linked into
   `TaskSmackTests` (see `tests/CMakeLists.txt`) for exactly this reason: it only needs `imgui.h`
   for types, and its OpenGL calls resolve against `glad_gl_core_33`, which is also linked (they
   only *succeed* with a GL context, which the tests don't have). Before reaching for extraction,
   check whether the file actually calls any `ImGui::`/`ImPlot::` function - if it doesn't,
   linking it directly gives real coverage of the actual production code instead of a parallel
   copy.
3. **Render the real code headless.** Add the `.cpp` to the "Source files under test" list in
   `tests/CMakeLists.txt` and run whole ImGui frames in a fixture:
   - `SetUp()`: `ImGui::CreateContext()` (plus `ImPlot::CreateContext()` for charts); set
     `io.IniFilename`/`io.LogFilename` to `nullptr`, `io.DisplaySize` and `io.DeltaTime`; set
     `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures` so ImGui builds and owns the font
     atlas itself with no renderer; then `io.Fonts->AddFontDefault()`.
   - Each frame: `ImGui::NewFrame()`, a fixed-size `ImGui::Begin(...)` window, call the code under
     test, `ImGui::End()`, `ImGui::Render()`.
   - Assert on the outcome: return values and the code's own state, ImGui state such as
     `ImGui::IsPopupOpen(...)`, or the geometry in `ImGui::GetDrawData()` (vertex/index counts,
     draw lists).
     To assert on the text drawn, call `ImGui::LogToBuffer()` before the code under test, read
     `GImGui->LogBuffer` (`imgui_internal.h`) after it, then `ImGui::LogFinish()`: logging captures
     every string rendered and lifts clipping (`tests/App/test_ProcessEnvironmentViewRender.cpp`).
   - `TearDown()`: destroy the ImPlot context, then the ImGui one.

   Examples: `tests/App/test_ProcessActionConfirmPopup.cpp` (modal lifecycle),
   `tests/App/test_ProcessPriorityViewRender.cpp` (the priority control),
   `tests/App/test_ProcessTableSettingsRoundTrip.cpp` (table settings loaded across frames),
   `tests/App/test_AboutDialogRender.cpp` (a dialog's size caps and scrolling body),
   `tests/App/test_KeyboardInputRender.cpp` (keys injected with `io.AddKeyEvent()` through
   `App/KeyboardInput.cpp`, the keyboard shortcuts' ImGui adapter, with keyboard navigation on),
   `tests/UI/test_FillPlotLayout.cpp`, and `tests/UI/test_ChartGeometryBudget.cpp` with
   `benchmarks/ChartGeometryScenes.h` (`HeadlessChartContext`, draw-data geometry counts).
   Colours come from the stub theme, which `ThemeStub.cpp` makes visible so charts emit geometry.

Before writing any of these tests, check whether the logic already exists as a shared, tested
helper - `TitleBarLayer.cpp` used to have its own private case-insensitive env-flag parser that
turned out to be a byte-for-byte duplicate of the already-shared, already-tested
`Core::isEnvFlagEnabled()` (`Core/EnvUtils.h`); deleting the duplicate and reusing the shared
helper was strictly better than writing a third copy of the same test.

### Testing Windows platform-probe code that touches real hardware/OS APIs

`Platform::Windows` probes read real hardware/OS state (battery status, GPU adapters, process
priority classes, disk instance names), so most of their branches only execute when the CI
runner happens to have the matching hardware or state - e.g. a battery-state switch never
reaches `Charging`/`Full` on a desktop CI runner with no battery, and NVML-backed GPU logic
never runs without a real NVIDIA driver installed. Two patterns handle this, chosen by whether
the untested logic needs OS handles or not:

1. **Pure logic: extract it into a small header taking primitives, the same technique as the
   ImGui one above but for hardware inputs instead of ImGui/SDL state.** Take the relevant OS
   struct's fields as plain integers (not the Win32 struct or handle type) so the header stays
   includable from a test file without pulling in COM interfaces - and without pulling in
   `windows.h` either, where the value being extracted doesn't need an SDK constant (some do:
   `WindowsProcessActionsMath.h` below still includes `windows.h` for the `*_PRIORITY_CLASS`
   constants it returns). See
   `WindowsPowerProbeMath.h` (`parsePowerStatus` takes `SYSTEM_POWER_STATUS`'s fields as
   `uint8_t`/`uint32_t` instead of the struct itself), `DXGIGPUProbeMath.h`
   (`isIntegratedGPUFromDesc` takes `DXGI_ADAPTER_DESC1`'s vendor/flags/memory fields instead of
   an `IDXGIAdapter1*`, avoiding COM mocking entirely), `WindowsGPUProbeMath.h`
   (`normalizeGPUName`/`gpuNamesMatch`), `WindowsProcessActionsMath.h` (`niceToPriorityClass`),
   and `WindowsDiskProbeMath.h` (`parsePhysicalDriveIndex`, alongside the pre-existing
   `clampNonNegativeQuadPart`). Writing tests against `parsePowerStatus` this way is what caught
   a real bug: `BATTERY_FLAG_UNKNOWN` (0xFF) also has the `BATTERY_FLAG_NO_BATTERY` bit (0x80)
   set, so the original bitmask-first check order made the `Unknown` battery state unreachable.
   Test a header that reaches no Windows header (directly or through its includes) in
   `tests/Platform/WindowsMath/test_<Header>.cpp`, listed in `WINDOWS_MATH_TEST_SOURCES`: those
   files are built on every platform, so Linux CI runs the Windows probes' arithmetic under its
   sanitizers and coverage as well (#1133). Such a test file must not include `windows.h` or use
   SDK macros (`NO_ERROR`, `ERROR_*`, `BATTERY_FLAG_*`); use the header's own constants. Tests that
   need a probe, Windows types or fakes of Windows APIs stay in `tests/Platform/test_Windows*.cpp`.
2. **Logic that must stay behind real OS handles/dynamically-loaded function pointers (e.g.
   NVML's `nvmlDevice_t`/function table): use a friend test-accessor struct**, declared as a
   single `friend struct FooTestAccessor;` line in the production class and defined only in the
   test file, so it never appears in the public API. Its static methods reach into the class's
   private members to inject fake state - see `NVMLGPUProbeTestAccessor` in
   `test_WindowsNVMLGPUProbe.cpp`, which injects a fake `NVMLFunctions` table and device handles
   so `enumerateGPUs()`/`readGPUCounters()`/`readProcessGPUCounters()`/`capabilities()` run
   deterministically without a real NVIDIA GPU. This is necessary (rather than Linux's
   dlopen-a-fake-`.so`-on-`LD_LIBRARY_PATH` trick) because `NVMLGPUProbe::loadNVML()`
   deliberately restricts `LoadLibraryExW` to `LOAD_LIBRARY_SEARCH_SYSTEM32` (security hardening
   so a portable installation cannot load an adjacent DLL) - a fake DLL placed elsewhere cannot
   be found. The accessor substitutes the backend *after* construction (the constructor's real
   `loadNVML()` still runs first; the accessor's `inject()` tears down whatever real backend it
   loaded before installing the fake one) rather than bypassing or weakening `loadNVML()` itself.
   The accessor struct must be declared directly in `namespace Platform` (not nested in the test
   file's anonymous namespace), since an
   anonymous-namespace type is a different entity than the one the friend declaration names.

### Visual validation without input (Process Details)

Agents must not inject clicks or keys, so five test-only environment variables (#1559, #172), read
once at startup, select a process, open a tab or open the Help or About window by themselves. With `TASKSMACK_SELECT_PID` or
`TASKSMACK_SELECT_NAME`, the process is selected when it first appears in a snapshot, exactly as a
click and the row menu's **Details** do, and Process Details opens. If it has not appeared after 20
snapshots, one warning is logged and nothing is selected. While any of the five variables is set,
the startup "Limited Data" notice is not shown, since it would cover the capture. Unset, the
variables do nothing.

| Variable | Value |
|---|---|
| `TASKSMACK_SELECT_PID` | A PID. Wins over `TASKSMACK_SELECT_NAME`. |
| `TASKSMACK_SELECT_NAME` | An executable name, e.g. `explorer.exe`; the first match. Case-insensitive on Windows, as Windows compares file names; exact on Linux. |
| `TASKSMACK_DETAILS_TAB` | `overview` (default), `gpu` or `network`. |
| `TASKSMACK_TAB` | The top-level tab to open: a tab's registered id (e.g. `Processes`, `ProcessDetails`) or its visible label (e.g. the hostname), else one of the aliases `system`/`machine` and `details`. Case-insensitive (for ASCII only on Linux). Wins over the Details tab a selection opens; an unknown name logs one warning. |
| `TASKSMACK_OPEN` | `help` (the Help window) or `about` (the About dialog), opened at startup. Case-insensitive; any other value logs one warning. |

Combined with `TASKSMACK_WINDOW` for a fixed size, then captured with `PrintWindow` (no input, and it
works while the window is covered):

```powershell
$env:TASKSMACK_WINDOW='1900x1000'; $env:TASKSMACK_SELECT_NAME='explorer.exe'; $env:TASKSMACK_DETAILS_TAB='overview'
$p = Start-Process .\build\win-debug\bin\TaskSmack.exe -PassThru
# Wait a few seconds, then PrintWindow($p.MainWindowHandle, hdc, PW_RENDERFULLCONTENT = 2) into a bitmap.
Stop-Process -Id $p.Id -Force   # not a graceful close, which would save your config.toml
```

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

`.clang-tidy` sets `WarningsAsErrors: '*'`, so any clang-tidy finding fails the run, locally and in CI's
blocking jobs. Fix the finding, or suppress it with a `NOLINT(check-name)` comment that says why. Before
this, clang-tidy exited 0 on warnings, so the CI job could never fail on one (#1089). CI runs clang-tidy
twice, both blocking: on Linux over what `tools/clang-tidy.sh` analyses (which excludes
`src/Platform/Windows/**`), and on Windows over what `tools/clang-tidy.ps1` analyses (everything but
`src/Platform/Linux/**`, so the Windows platform code and the `_WIN32` branches of shared files) (#1233).
Run the Windows script before pushing a change to Windows-only code. The Windows script skips one check,
`clang-analyzer-optin.core.EnumCastOutOfRange`, for an MSVC STL false positive it can't suppress in our code.

Naming is enforced by `readability-identifier-naming` in `.clang-tidy`: `PascalCase` classes, structs,
enums, enumerators, namespaces and type aliases; `camelCase` functions, methods and parameters;
`m_PascalCase` private and protected members (`m_CurrentFontSize`, not `m_currentFontSize`); and
`UPPER_SNAKE_CASE` constants by convention. Public data members of snapshot structs are plain
`camelCase` with no prefix.

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

The report counts all of `src/`, not just the files linked into `TaskSmackTests`: the coverage
scripts pass the instrumented app binary (`build/<preset>/bin/TaskSmack`) to `llvm-cov` as an extra
`-object`, so every file the tests never execute (the ImGui panels and layers, `Theme.cpp`,
`main.cpp`, ...) shows up at 0% instead of silently dropping out of the denominator (#1131).

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

> **Note (display-dependent tests):** The `ApplicationTest`, `WindowTest`, `IconLoaderGLTest` and
> `AssetPathTest.FindAssetsDirIsStableAcrossCalls` tests need a GL 3.3 core context. They skip only
> when the up-front display probe finds none; once it passes, a construction exception fails the
> test. Linux CI runs `ctest` under `xvfb-run` with Mesa and sets `TASKSMACK_REQUIRE_DISPLAY=1`,
> which turns a failed probe into a failure too, so a broken CI display can't hide as skips (#1132).

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

UndefinedBehaviorSanitizer halts on the first error. The preset compiles with
`-fno-sanitize-recover=undefined`, and `ctest --preset asan-ubsan` also injects:

- `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:$penv{UBSAN_OPTIONS}`

As with `LSAN_OPTIONS`, any caller-provided `UBSAN_OPTIONS` are appended and take
precedence. Without these, UBSan printed `runtime error:` and let the test pass,
and `ctest --output-on-failure` hid the report, so undefined behaviour could
never fail a run (#1090).

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

ClusterFuzzLite continuously exercises three parsers with libFuzzer and AddressSanitizer:

| Target | Entry point | Seed corpus |
|--------|-------------|-------------|
| `fuzz_proc_parsing` | the allocation-free `/proc` numeric parsers (`Platform/Linux/ProcParsing.h`) | none |
| `fuzz_user_config` | `App::UserConfig::parseSettings`: toml++ plus the `config.toml` schema, as `load()` reads it | `tests/fuzz/corpus/fuzz_user_config/` |
| `fuzz_theme_loader` | `UI::ThemeLoader::loadThemeFromString`: toml++ plus every theme colour lookup, as `loadTheme()` reads a file | `assets/themes/*.toml` and `tests/fuzz/corpus/fuzz_theme_loader/` |

Pull requests that change `tests/fuzz/**`, `.clusterfuzzlite/**`, the `cflite_*.yml` workflows,
any fuzzed parser (`ProcParsing.h`, `UserConfig.{cpp,h}`, `UserConfigHelpers.h`,
`ThemeLoader.{cpp,h}`, and the types they fill, `Theme.h` and `ProcessColumnConfig.h`), the fuzz build's other inputs
(`assets/themes/**`, the theme seed corpus, and `tests/Mocks/ThemeStub.cpp`), or the dependency
pins and patches (`cmake/Dependencies.cmake`, `cmake/patches/**`) run a short
code-change fuzzing job (`cflite_pr.yml`'s path filter); pushes to `main` touching the same paths
refresh the baseline build (`cflite_build.yml`). Separate weekly jobs perform a longer batch run and prune the resulting corpus.
`.clusterfuzzlite/build.sh` builds every target (ClusterFuzzLite runs each binary it leaves in
`$OUT`), so a new target is a `tests/fuzz/fuzz_<name>.cpp` plus one `build_fuzzer` line there. The
targets compile against the header-only toml++, spdlog, Dear ImGui and ImPlot at the commits
`cmake/Dependencies.cmake` pins, with the same `cmake/patches/` applied (`.clusterfuzzlite/fetch-deps.sh`).

To build and run the targets locally with Clang, from the repo root:

```bash
CXX=clang++-22 CXXFLAGS="-O1 -g -fsanitize=address,fuzzer-no-link" \
  LIB_FUZZING_ENGINE=-fsanitize=fuzzer OUT=build/fuzz .clusterfuzzlite/build.sh
mkdir -p build/fuzz/corpus-config && unzip -o build/fuzz/fuzz_user_config_seed_corpus.zip -d build/fuzz/corpus-config
./build/fuzz/fuzz_user_config -max_total_time=60 build/fuzz/corpus-config
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

Both scripts exit non-zero when the benchmark binary fails or crashes (#1423). A partial result
file is still redacted (or deleted when it is too truncated to parse), so it never keeps the host
name, but it is not reported as usable; output that cannot be redacted after a successful exit is
deleted too, and the script fails.

### Benchmark Output

Each `bench.sh` / `bench.ps1` run writes two files to `perf-data/`. The result name is claimed
atomically before the benchmark starts, so a run started in the same second as another --
concurrently or not -- appends `-2`, `-3`, ... to the timestamp rather than overwrite it. A
user or host name in the preset (standing alone between separators, the preset's own `-`, `.`
and `_` included) becomes `user` / `host` in both file names, and `<user>` / `<host>` in the
manifest's `preset`, so no file name or manifest field carries it:

- `<preset>-<timestamp>.json` -- Google Benchmark's JSON with **every repetition** (`run_type:
  "iteration"`) plus the `mean`/`median`/`stddev`/`cv` aggregate rows. The scripts deliberately do
  not pass `--benchmark_report_aggregates_only`, so distributions can be re-analysed; only the
  console shows aggregates alone. `tools/check-benchmark-regression.py` compares the `median`
  rows. `context.host_name` is redacted and `context.executable` reduced to its file name.
- `<preset>-<timestamp>.manifest.json` -- a provenance sidecar (#1424) with the same field names
  from both scripts: `git` (commit, branch -- a user or host name in it, standing alone between
  separators that include the branch's own `-`, `.`, `_` and `/`, becomes `<user>` / `<host>`,
  so `feature/benchuser-fix` is recorded as `feature/<user>-fix` -- and dirty flag for tracked
  files; no diff), `binary` (file
  name and SHA-256), `build` (build type, generator, compiler name/id/version, IPO and the C++
  flags' hashes, read from the build tree's `CMakeCache.txt` -- the nearest one above the binary,
  so a multi-config `bin/<Config>/` binary is found too, with `<Config>` as its build type; the
  build type is the `config` of the build information described below when it has one, so a tree
  reconfigured from Release to Debug without a rebuild still reports its Release binary -- the
  compiler id/version from the `CMakeFiles/<version>/` of the cache's own CMake version, or
  unknown; `ipo` is what that configuration of the `TaskSmackBenchmarks` target is built with,
  read from the `TaskSmackBenchmarks.buildinfo.json` that `benchmarks/CMakeLists.txt` generates
  for each configuration and copies next to the binary whenever it links, so it always describes
  that binary (`ipo_source` `buildinfo`) -- the cached
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION` can say `OFF` while `TASKSMACK_ENABLE_IPO` turns IPO on,
  and a multi-config generator can set it per configuration -- with the cached
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION` and then `TASKSMACK_ENABLE_IPO` as fallbacks for older
  build trees, `ipo_source` naming the entry used), `benchmark` (the arguments passed,
  allowlisted or hashed as below), `exit_code`, and
  `machine`, an anonymized machine class (CPU model, logical core count, OS name/version,
  architecture).
  - The benchmark arguments are recorded as written only when they are Google Benchmark options
    whose values are safe by construction: numbers (`--benchmark_repetitions`,
    `--benchmark_min_time`, `--benchmark_min_warmup_time`, `--v`), booleans (the aggregates,
    random-interleaving, tabular-counters, dry-run and list-tests options, bare or with a value),
    enumerations (`--benchmark_time_unit`, `--benchmark_format`, `--benchmark_out_format`,
    `--benchmark_color`) and the `--benchmark_filter` regex. The script's own `--benchmark_out`
    keeps only its file name. A value that fails its pattern, and every other argument
    (`--benchmark_context=...`, unknown options), is recorded as `<name>=sha256:<hex>` -- a hash of
    the raw value -- or `sha256:<hex>` of the whole argument when it has no `--name=value` form. As
    with the flags, runs stay comparable on their arguments without the manifest recording the
    paths or other free text in them.
  - The compiler flags are recorded only as `build.cxx_flags_sha256` and
    `build.cxx_flags_config_sha256`: SHA-256 of the `CMAKE_CXX_FLAGS` and
    `CMAKE_CXX_FLAGS_<CONFIG>` values as CMake reads them from `CMakeCache.txt` (UTF-8, no other
    normalization; `<CONFIG>` is the build type upper-cased as CMake does, so a custom type such as
    `ASan-UBSan` is found; `null` when the entry is absent, the hash of the empty string when it is
    present but empty). `benchmarks/CMakeLists.txt` computes the same hashes at configure time
    into the build information copied next to the binary when it links, and the writers prefer
    those (`build.cxx_flags_source` `buildinfo`), so flags reconfigured without a rebuild are not
    reported for the old binary; a tree without them is read from the cache (`cache`). Two runs
    can be compared on them -- equal hashes, equal flags -- without the
    manifest carrying the include directories, profile files and prefix maps the flags name,
    which sit under user profiles and checkouts.
  - It records no host name, user name, user-profile path, process list or other command line; a
    final pass over the free-form fields (args, branch, compiler file name, preset and
    result names, CPU model) replaces any remaining home-directory prefix with `<home>` --
    whatever its length, so a home of `/ab` too; never a root or a bare drive -- and the
    host name (short and FQDN) and user name (3+ characters, standing alone between separators)
    with `<host>` and `<user>`. Validated categorical fields (OS name/version, architecture,
    compiler id/version, generator, the flag hashes, a standard build type -- Debug, Release, RelWithDebInfo,
    MinSizeRel --, schema fields, numbers and booleans) are left alone, so a host named `Linux` or
    a user named `clang` cannot rewrite them; a custom build type is scrubbed, its own `-`, `.`
    and `_` bounding a name too (`ASan-benchuser` becomes `ASan-<user>`). The OS version is
    still checked for a user or host name between its own `-`, `.` and `_`, as a Linux kernel
    built with `CONFIG_LOCALVERSION` reports one (`6.8.0-benchhost` becomes `6.8.0-<host>`, in
    `machine.label` too). Outside a git
    checkout, inside another repository's tree (a source archive unpacked in a checkout), or
    without git, the `git` fields are all `null`. The manifest is written before the benchmark
    starts (`exit_code` `null`) and only the exit code is added afterwards, so the git state, build
    configuration and binary hash describe what was launched. `bench.sh` writes it with
    `tools/bench-manifest.py`.

For the script tests, `bench.ps1 -BenchmarkBinary <path> -OutputDirectory <dir>` and `bench.sh`'s
`TASKSMACK_BENCH_BIN` / `TASKSMACK_BENCH_OUT_DIR` environment variables point the scripts at a
stub binary and a scratch directory (`tools/test-bench.ps1`, `tests/tools/test_bench_sh.py`).
A relative binary path, a bare name included, is relative to the current directory (the
PowerShell location for `bench.ps1`) and is never looked up on `PATH`: the file that is checked
and hashed is the one that runs. They are also the only way to move the output: an extra `--benchmark_out` or
`--benchmark_out_format` argument is refused before the benchmark starts, because the redaction
and the manifest only look at the file the script chose.

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

### CI Benchmark Regression Gate

PR CI (`ci.yml`'s Linux Release job) only *builds* `TaskSmackBenchmarks` -- it never runs it -- so a
change that breaks the benchmark build fails the PR (#1348). Timing runs and the regression gate below
live only in `heavy-checks.yml`.

`heavy-checks.yml`'s `benchmark-regression` job runs on every push to `main`, gating against
`perf-data/linux-ci-baseline.json` via `tools/check-benchmark-regression.py` (40% threshold,
comparing medians of `tools/bench.sh`'s 10 repetitions per benchmark) -- a failure here **fails
the job** (`exit 1`), unlike the informational-only mode this ran in before #683. The script also
enforces a `--min-coverage` floor (default 90%): a benchmark missing from the current run, or one
with no usable timing data on either side, counts against coverage instead of being silently
ignored (see #871 -- this closed three concrete false-pass paths: a missing baseline benchmark,
a non-finite/`NaN` timing, and comparing two different timing fields for the same benchmark).
A slowdown also has to exceed an absolute noise floor, `--min-abs-delta-ns` (default 1.0ns), to
count: a sub-nanosecond microbenchmark such as `BM_Numeric_ToDouble_Int` moving 0.4ns -> 0.6ns
reads as +50% but is timer noise (#1322). Pass `--min-abs-delta-ns 0` to gate on percentage alone.
A benchmark that skips itself on purpose (`state.SkipWithMessage(...)`, `"skipped": true` in the
JSON) on either side is reported as *not measured* and left out of coverage entirely, numerator and
denominator; a `SkipWithError` (`"error_occurred": true`) on either side still counts against it, even if the other side skipped. The
`BM_GPUProbe_*`/`BM_GPUModel_*` benchmarks skip this way when the real probe finds no GPU, as on the
hosted runner, instead of timing an empty probe's early return (#1420).

This is a *separate* baseline from `perf-data/linux-baseline.json` above, deliberately: that one
was recorded on a local developer machine (10 cores @ 3.7 GHz) for local `tools/bench.sh`
comparisons, and comparing it against hosted `ubuntu-24.04` runners (shared 4-vCPU machines)
flagged machine-class differences as regressions on every single run (65/84 benchmarks
"regressed," up to +813%, including pure-arithmetic microbenchmarks -- see #683). Comparing
CI-recorded-vs-CI-recorded instead removes that machine-class variance entirely.

Removing machine-class variance does not remove all hosted-runner noise, though: capturing the
baseline required three separate `heavy-checks.yml` dispatches to characterize, and the CPU
performance ubuntu-24.04 hosted runners actually deliver appears **bimodal**, not a smooth
noise distribution around one typical value -- two of the three captures were closely
consistent with each other (within ~16% on every benchmark), while the third was consistently
20-60% faster across essentially all 78 comparable benchmarks simultaneously (a uniform,
systemic shift, not the mixed-sign scatter true noise would produce). The committed baseline
intentionally uses one of the two *slower*-class samples: comparing a future run against it can
only ever show a large "improvement" if that run lands on the faster class (never a false
regression, since improvements never fail the gate), whereas the reverse choice (a fast-class
baseline) would make roughly half of all future runs report a 50-150%+ false "regression" purely
from landing on the slower class. This trades some sensitivity to genuinely small/moderate
regressions for eliminating false positives outright -- which was the explicit problem #682 was
created to solve (a red gate nobody could trust). Catching smaller regressions reliably despite
this would need either a same-job speed-calibration measurement to normalize away the class
effect, or requiring two independent runs to agree before failing; neither is implemented here.

**Refreshing `perf-data/linux-ci-baseline.json`** (only needed when hosted-runner hardware
changes, e.g. a `ubuntu-24.04` image update measurably shifts baseline timings, or after a
deliberate, reviewed performance change that the gate should treat as the new normal):

1. Manually trigger `heavy-checks.yml` via `gh workflow run heavy-checks.yml --ref <branch>` (or
   the Actions tab's "Run workflow" button), and wait for the `benchmark-regression` job to
   finish.
2. Download its `benchmark-results` artifact and extract `perf-data/benchmark-current.json`:
   ```bash
   gh run download <run-id> --name benchmark-results --dir /tmp/benchmark-results
   ```
3. Review the new numbers against the old baseline before replacing it -- a baseline refresh
   should be a deliberate, reviewed change, not a way to silently paper over a real regression.
   Given the bimodal runner behavior above, if every benchmark shifted by roughly the same
   percentage in the same direction, that's very likely a runner-class difference, not a real
   change -- dispatch one more run and compare it to the candidate before deciding which to
   keep, rather than trusting a single capture.
4. Replace the baseline and commit it in its own PR (not bundled with unrelated changes):
   ```bash
   cp /tmp/benchmark-results/perf-data/benchmark-current.json perf-data/linux-ci-baseline.json
   git add perf-data/linux-ci-baseline.json
   git commit -m "chore(perf): refresh CI benchmark baseline"
   ```
5. Push and confirm the next `heavy-checks.yml` run passes against the refreshed baseline.

### Available Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_HistoryBuffer_*` | `HistoryBuffer` ring operations (push, access, copyTo) |
| `BM_HistoryBuffer_MemoryFootprint` | Memory usage tracking for history buffers |
| `BM_ProcessModel_*` | Process enumeration and snapshot computation |
| `BM_ProcessModel_MemoryGrowth` | Memory growth over repeated refresh cycles |
| `BM_ProcessProbe_Enumerate` | Raw OS API performance |
| `BM_ProcessProbe_EnumerateSynthetic*` | `LinuxProcessProbe::enumerate()` over a synthetic 5,000-process /proc (Linux only), so runs compare like for like: steady state, and a pass that also rebuilds the socket inode-to-PID map from its fd walk (`...Rebuild`) |
| `BM_SystemModel_*` | System metric sampling and history accessor performance |
| `BM_SystemModel_MemoryGrowth` | Memory growth over repeated `refresh()` calls exercising the full probe read, delta computation, and history append path |
| `BM_SystemProbe_Sample` | Raw OS system probe API performance |
| `BM_Format_*` | UI formatting functions |
| `BM_NetlinkSocketStats_*` | Netlink INET_DIAG socket query performance (Linux only) |
| `BM_StorageModel_*` | Storage probe/model sampling, history accessor, and per-disk snapshot performance |
| `BM_StorageModel_MemoryGrowth` | Memory growth over repeated `sample()` cycles |
| `BM_GPUProbe_*`, `BM_GPUModel_*` | GPU probe enumeration, counter reads, model refresh, and history accessor performance (skipped on a machine with no GPU, #1420; the mock-probe `BM_GPUModel_History_Publish`/`BM_GPUModel_Concurrent_PublicationWait` below always run) |
| `BM_GPUModel_MemoryGrowth` | Memory growth over repeated GPU `refresh()` cycles |
| `BM_Numeric_*` | Micro-benchmarks for `toDouble`, `clampPercentToFloat`, `narrowOr`, and mixed process-table workload |
| `BM_ChartWidgets_*` | `UI::Widgets` chart helpers: `computeAlpha` smoothing, `tailAlignedSpan` history-window selection, and the `formatAxisLocalized`/`formatAxisBytesPerSec` axis-label formatters — the layer the Windows ETW app-trace (perf-plan-574 / issue #574) flagged as expensive but that previously had no Linux-runnable coverage |
| `BM_ChartGeometry_*` | One whole headless ImGui+ImPlot frame (`NewFrame()` through `Render()`, no window or GL) of the real `ChartWidgets.h` charts with fixed data: the stacked CPU chart at full history, the per-core grid (16 cores, and 64 narrow ones whose point budget follows the plot width, #1411), the memory chart, and an uncached min/max-reduced 18k-sample line. Reports `vertices`/`indices`/`draw_lists`/`draw_cmds` counters; the same scenes (`benchmarks/ChartGeometryScenes.h`) are held to a vertex/index budget by `tests/UI/test_ChartGeometryBudget.cpp`, which gates every PR (#1421) |
| `BM_*_FullHistory/*`, `BM_*_Cardinality/*` | Domain model `publish()`/`publication()` fed from `tests/Mocks` probes at the limits: history held at 300/3k/18k samples (18k = 1800 s at 100 ms), and many cores, interfaces, disks or processes (#1422) |
| `BM_SystemModel_Concurrent_PublicationWait/*`, `BM_GPUModel_Concurrent_PublicationWait/*` | How long a UI-style `publication()` call waits when it lands on a publish in another thread, with optional extra reader threads: the exclusive lock's hold time before #868, a pointer swap since (#1422, #868) |

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
Default preset is `profile` for app mode and `benchmark` for bench mode. Every run prints, and
writes to its log, the preset, build directory, build type, compiler and `CMAKE_CXX_FLAGS*` entries
it profiled. Those cache entries miss `add_compile_options()`/`target_compile_options()` flags such as
a `TASKSMACK_MARCH` `-march`, `-stdlib=libc++` and the release hardening flags. So each run also
prints the real compile flags of one of the profiled binary's `src/` files (`src/main.cpp` for the
app), read from the build's `compile_commands.json` with `python3`, and logs that file's full
compile command. Every preset exports `compile_commands.json`. If the file or `python3` is missing,
the run says so and logs the cache entries alone.

App mode profiles steady state, not startup (#1371). It launches TaskSmack, waits for it to log
`Entering main loop` (up to 30 s) plus a warm-up (`--warmup`, default 5 s), and only then attaches
`perf record -p`. It records until you close TaskSmack, or for `--duration` seconds and then closes
it. Ctrl+C also stops the capture and closes TaskSmack. `--include-startup` keeps the old behavior:
TaskSmack runs under perf from launch. The run fails if TaskSmack exits before recording starts,
exits before `--duration` elapses, or exits with a non-zero code. When the script closes TaskSmack
itself (after `--duration` or Ctrl+C), it sends SIGTERM, and TaskSmack quitting cleanly with code 0
passes. Needing the SIGKILL fallback after 10 s, or any other exit code, fails the run. In every app
capture, `--include-startup` included, TaskSmack's own stdout and stderr go to
`perf-data/perf-app-<timestamp>-app.log`. The `profile` preset keeps frame pointers for better stacks;
pass `--preset release` to profile the shipped build's code generation.

```bash
# App trace — steady state after a 5 s warm-up; exercise the app, then close it
./tools/profile-perf.sh app

# Unattended app trace — 10 s warm-up, record 30 s, then close automatically
./tools/profile-perf.sh app --warmup 10 --duration 30

# Include startup (fonts, themes, first enumeration) in the profile
./tools/profile-perf.sh app --include-startup

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

**Profile one benchmark at a time for accurate hotspot attribution.** Google Benchmark scales
each benchmark's iteration count (not its wall-clock share) to fill `--benchmark_min_time`, so a
cheap per-call benchmark gets looped far more times than an expensive one run alongside it. Since
`perf`'s sampling is time-based, that gives the cheap benchmark equal or greater representation in
the profile regardless of its real-world cost — confirmed directly: profiling the default
multi-benchmark filter on a GPU-less machine attributed ~40% of total samples to near-zero-cost GPU
no-op benchmarks, drowning out the genuinely expensive process/system-probe work measured alongside
them. `profile-perf.sh bench` detects this automatically and warns (listing every matched
benchmark) whenever `--bench-filter` matches more than one benchmark; pass a filter that matches
exactly one (e.g. `--bench-filter 'BM_ProcessModel_Refresh$'`) to get a clean, single-function
capture.

**Flamegraph generation** requires the Brendan Gregg FlameGraph scripts, which
`tools/setup-dev.sh` clones to `~/opt/FlameGraph` automatically (unless run with `--minimal`).
To set up manually instead:
```bash
git clone https://github.com/brendangregg/FlameGraph ~/opt/FlameGraph
export PATH="$HOME/opt/FlameGraph:$PATH"
# analyze-perf.sh auto-detects them and generates the SVG
./tools/analyze-perf.sh perf-data/perf-app-<timestamp>.data
```

**WSL2 note (tools/kernel version mismatch):** `perf` requires a kernel-matched tools
package. If you see a version mismatch warning, run
`sudo apt install linux-tools-$(uname -r) linux-tools-generic` or profile on a native
Linux machine / GitHub Actions runner.

**WSL2 note (no hardware counters — a separate issue):** WSL2's hypervisor does not pass
through real hardware performance counters, so the default `cycles` event silently
records zero samples instead of erroring — you'd only discover it later from
`perf report`/`analyze-perf.sh` failing with "has no samples". `tools/profile-perf.sh`
detects this automatically and falls back to the `cpu-clock` software event, printing a
notice when it does; it also verifies the capture actually contains samples and fails
loudly if not. The same fallback applies to most containers and some cloud VMs/CI runners.

### Linux — Heap allocation profiling (heaptrack)

Use `tools/profile-heap.sh` to find hot-path heap allocations that don't show up
in CPU profiles. Approximately 2–3× runtime overhead (vs. Valgrind's ~50×).

```bash
# Install (already done for you by tools/setup-dev.sh, unless run with --minimal)
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
Run captures from a **normal (non-elevated) terminal**. `app` and `bench` elevate only the WPR
collector (one UAC prompt) and run the target at ordinary-user integrity, because an elevated
target sees and does different things, which skews what is measured (#872). The target's
measured integrity level is written to `perf-data/etw-<mode>-<timestamp>.manifest.json`. The
scripts build before prompting for elevation. For resize diagnosis, use the `resize`
procedure below.

```powershell
# App trace — exercise the app, then close it (defaults to win-release, the preset releases ship)
pwsh tools/profile-etw.ps1 app

# Longer warm-up before recording, or record startup deliberately
pwsh tools/profile-etw.ps1 app -DurationSeconds 45 -WarmupSeconds 15
pwsh tools/profile-etw.ps1 app -DurationSeconds 20 -IncludeStartup

# Dry run of the launch/warm-up/crash checks, with no WPR session and no UAC prompt
pwsh tools/profile-etw.ps1 app -DurationSeconds 10 -SkipTrace

# Benchmark trace
pwsh tools/profile-etw.ps1 bench
pwsh tools/profile-etw.ps1 bench -BenchmarkFilter 'BM_ProcessModel_Refresh$'
pwsh tools/profile-etw.ps1 bench -BenchmarkFilter '.*'   # profile the entire benchmark suite

# Use win-profile for symbol-rich follow-up attribution
pwsh tools/profile-etw.ps1 app -Preset win-profile

# Unattended/scripted app trace — run for a fixed window and close automatically
pwsh tools/profile-etw.ps1 app -DurationSeconds 45

# Deliberately elevated target (the old behaviour), labelled etw-app-elevated-*
pwsh tools/profile-etw.ps1 app -DurationSeconds 45 -ElevatedTarget

# Analyze a captured trace
pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-<timestamp>.etl

# Skip function decoding (faster, module-level only)
pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-<timestamp>.etl -SkipFunctions

# Also measure the recording's own cost inside TaskSmack (ETW logging path), optionally
# within an interval in microseconds from trace start
pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-<timestamp>.etl -Overhead
pwsh tools/analyze-etw.ps1 -TracePath .\run\trace.etl -SymbolPath .\run\bin -Overhead -RangeStartUs 12000000 -RangeEndUs 13064000

# Optional VTune workflow if installed
vtune -collect hotspots -- .\build\win-profile\bin\TaskSmack.exe
```

Notes:
- `wpr`, `xperf`, and `wpa` ship with the Windows Performance Toolkit (install via Windows SDK).
- ETW recording requires elevation. By default `profile-etw.ps1` elevates only a separate WPR collector, not the target, and validates all output artifacts before returning; `-ElevatedTarget` is the explicit opt-in that runs the target elevated too. From an elevated terminal the script refuses unless `-ElevatedTarget` is passed, since the target would inherit the elevation.
- Captures use unique WPR instance names and never cancel an existing recording. If
  another recorder prevents startup, leave it alone and coordinate with its owner.
- App mode defaults to `win-release`, the preset `.github/workflows/release.yml` builds and ships, so
  the profile measures the binary users run. The preset and the compile flags its build tree was
  configured with (from its `CMakeCache.txt`) are logged at the start and recorded in the manifest.
  Use `win-profile` when you need function-level symbol attribution; `-Preset win-optimized`
  (LTO, `-march=x86-64-v3`) profiles that opt-in build, which is not what ships.
- App mode excludes startup: it launches the app, waits for its main window plus `-WarmupSeconds`
  (default 5), and only then starts the trace (prompting for UAC at that point). With
  `-DurationSeconds` the trace is stopped before the script closes the app. `-IncludeStartup`
  starts the trace before the launch instead.
- A crashed run fails (#1186): the app exiting during warm-up, exiting before a `-DurationSeconds`
  window ends, or being closed with a nonzero exit code stops the trace, writes the manifest (with
  the exit code and reason), exits nonzero and prints no `TRACE=` line.
- Function decoding against `win-release`/`win-optimized` binaries may be limited (no debug info); `analyze-etw.ps1` degrades gracefully with an explanatory message.
- `analyze-etw.ps1` judges every trace before you rely on it (#873) and prints, and writes to
  `<trace>-analysis.json`, a **Valid / Degraded / Invalid** verdict with reasons:
  - **Lost events and buffers**, as a count and a share of all events. More than 1% lost, or
    any lost buffer, is Invalid (`-MaxLostEventsPct`).
  - **Symbol identity**: the binary in `-SymbolPath` must be the build the trace captured; its
    PDB signature and age are compared with the ones the trace recorded. A mismatch is flagged.
  - **Unresolved functions**: the share of the target module's samples with no function name.
    More than 10% is Invalid (`-MaxUnresolvedPct`).

  An Invalid trace is still exported for inspection, but the script exits with code 3 unless
  `-AllowInvalid` is passed. In the reports, `ProcessSharePct` is a share of the process's
  sampled CPU and `AppCodeSharePct` a share of `TaskSmack.exe!*` samples only; `CpuMs` is
  absolute sampled CPU.
- `-Overhead` (#931) reports how much of the process's sampled CPU was ETW's own logging path,
  in `<trace>-overhead.json`, so a capture can answer "how much of this was my own measurement?"
  It downloads only the kernel PDB the trace recorded, into `perf-data\KernelSymbols`; if the
  kernel's functions still do not resolve, it says so instead of reporting a low share.
- Capture uses `tools/TaskSmackCPU.wprp`, a custom WPR profile with larger buffers than the
  built-in `CPU` profile, to avoid the "trace has dropped N events" warning that the built-in
  profile produces on machines with many logical cores under system-wide sampling.
- The default `-BenchmarkFilter` for `bench` mode covers every probe/model refresh path plus
  the PDH per-process GPU path and core `HistoryBuffer` ring operations (`BM_HistoryBuffer_*`); pass `.*` to profile
  the entire suite instead.
- `bench` checks the filter first, like `tools/profile-perf.sh` (#874). A filter matching no
  benchmark fails before recording. One matching several warns and lists them: each benchmark
  gets the same minimum time, so a cheap one is looped more and gets as many samples as an
  expensive one. For hotspot attribution, match exactly one benchmark. The matched names are in
  the manifest.

#### Same-run resize diagnostics (normal-user app, elevated collector)

`profile-etw.ps1 resize` uses PowerShell 7 and four explicit phases. Only **Collect**
requires elevation; it never launches the app or executes commands from the run manifest.
Prepare/App/Check refuse elevated terminals. This avoids the workload-elevation confound
without replacing the legacy profiling modes. No driver changes, TDR settings, GPU
synchronization queries, or automatic cancellation of other recordings are involved.

A run directory inside the worktree must be git-ignored, and `Prepare` refuses one that
isn't. `Prepare` writes into it before CMake evaluates `GIT_SOURCE_STATE`, so a tracked
run directory stamps the captured binary as `configureSourceState=dirty` and makes
`checkout-status.txt` report the capture's own output as a source change -- destroying the
provenance the capture exists to establish. `perf-data/resize-*/` and `build/` are already
ignored; anywhere outside the repository is also fine.

```powershell
# Normal-user terminal, repository root. Pick a NEW directory for EVERY run.
$run = Join-Path $PWD 'perf-data\resize-001'
pwsh -File tools\profile-etw.ps1 resize -Phase Prepare -RunDirectory $run -Preset win-profile
# Use win-optimized for production-like timing; it may have no PDB.
# -SkipBuild snapshots existing files, but cannot establish their source provenance.

# Separate elevated terminal, repository root; set $run to the SAME absolute path.
pwsh -File tools\profile-etw.ps1 resize -Phase Collect -RunDirectory $run -DurationSeconds 90

# Once Collect prints "Recording", run in the ORIGINAL normal-user terminal:
pwsh -File tools\profile-etw.ps1 resize -Phase App -RunDirectory $run
# Idle briefly, repeatedly resize edges/corners for 20-30 seconds, then close the app.
# Let the collector's timer expire (default 180s; allowed 15-600s).

# After BOTH commands finish, normal-user terminal:
pwsh -File tools\profile-etw.ps1 resize -Phase Check -RunDirectory $run
```

**Provider set (`-ProviderSet`).** The default `Focused` uses `tools/TaskSmackResize.wprp`, which
keeps exactly what attribution needs -- `CSwitch`, `ReadyThread` and `SampledProfile` with stacks on
all three, plus `DxgKrnl`, `Dwm-Core`, `DXGI` and `Kernel-EventTracing` -- and deliberately omits
`GeneralProfile.Verbose`'s stack-walked `Microsoft-Windows-Win32k` provider and its
`DiskIO`/`DPC`/`Interrupt`/fault keywords. Note that basing a profile on the built-in `CPU.Verbose`
is *not* sufficient: its own event collector still enables Win32k with stacks, which is why this
profile declares its collectors explicitly.

`-ProviderSet Verbose` restores the earlier `GeneralProfile.Verbose` + `GPU.Verbose` +
`DesktopComposition.Verbose` set. It is an explicit opt-in rather than the default because, measured
inside a real 1064 ms stall, that set accounted for **48.1%** of the sampled CPU it was recording
(dominated by `EtwpLogKernelEvent`/`EtwpReserveTraceBuffer`/`KeQueryPerformanceCounter`) -- it was
measuring its own logging more than the stall. On an identical 20-second workload the focused
profile produced a **93 MB** ETL against **1026 MB**, both loss-free, with the ETW share of sampled
CPU down to **1.8%**. See #912.

The resolved providers, keywords and stack settings of whichever profile was used are retained in
the run directory (`profiledetails-*.txt` for the focused profile, `<ProfileName>.txt` for the
built-ins), so a capture's configuration is recoverable from its own artifacts.

**Ring buffering for rare stalls (`-Buffering Ring`).** File mode records a fixed window and
depends on the stall happening inside it, which is the wrong shape for a freeze that occurs
once in many minutes. `-Buffering Ring` omits WPR's `-filemode` so events accumulate in an
in-memory ring buffer; you resize until you actually *see* a freeze, then request the save and
only the buffer's remaining contents are written. Elevated terminal:

```powershell
pwsh -File tools\profile-etw.ps1 resize -Phase Collect -RunDirectory $run -Buffering Ring -DurationSeconds 600
```

`-Phase App` blocks until the app exits, so `Save` needs a terminal of its own: use a THIRD
normal-user terminal (this writes the buffer closest to the freeze), or close the app first
and then run `Save` immediately, accepting the app-shutdown time as extra ring depth
consumed. Start `-Phase App` as above; the moment a freeze is visible:

```powershell
pwsh -File tools\profile-etw.ps1 resize -Phase Save -RunDirectory $run   # writes trace.etl now
```

Close the app afterwards, then run `-Phase Check`. `Save` is normal-user, ring-only and
single-shot; the collector still stops only the recording it started, and the bounded deadline
still applies if no save is requested.

Two consequences are specific to ring mode and Check enforces the first, not the second. The
app normally exits *after* the ETL is written, so Check applies a coverage rule (app started
inside the recording and before the save) rather than file mode's stricter containment rule.
And the buffer is memory-bounded and silently overwrites its oldest events, so no timestamp
can show whether the ETL still reaches back to the stall -- Check marks that as required
manual verification in WPA rather than claiming coverage.

Prepare copies the executable, PDBs, sidecar DLLs and assets into `binary\` and records
SHA-256/length for each file, the preset, CMake cache, generated `version.h`, and capture-time
checkout identity. App runs that snapshot, not mutable build output; Check rechecks its
hashes. PDB absence is explicit: an optimized timing run is not a symbol-rich attribution
run, and a same-named PDB from a different build is not a substitute. CMake embeds the full
commit and clean/dirty/unknown state **at configure time**; reconfigure after source changes.
Neither capture-time HEAD nor `-SkipBuild` proves what produced an old executable. Preserve
the exact snapshot and confirm symbol GUID/age matching in WPA.

The shared directory retains `run.json`, `collector.json`, `app.json`, `stdout.log`,
`stderr.log`, `trace.etl`, command outputs and exit-code/timestamp sidecars. Identity
records include UTC/QPC, PID and actual launcher elevation; `*-token.txt` retains
`whoami /all` including integrity level. The app inherits the non-elevated launcher's token
(no RunAs; TaskSmack's manifest is asInvoker). These artifacts include machine/process
information: inspect them before sharing.

Collect requests whichever provider set `-ProviderSet` selects (see above): by default the single
`tools/TaskSmackResize.wprp!TaskSmackResize` profile, whose resolved definition is retained as
`profiledetails-1.txt`; with `-ProviderSet Verbose`, the three built-in profiles, retained as
`*.Verbose.txt`. Either way the retained definition describes the variant actually recorded --
`-profiledetails` is passed `-filemode` only when the capture uses file mode, since that flag is
what selects a profile's `.File` variant over its `.Memory` one. The focused profile supplies
CSwitch/ReadyThread **stacks**, not just sampled CPU, plus DxgKrnl/DWM/DXGI. Profile
availability/buffer sizes vary with the installed WPT version. Do not assume a requested provider
emitted usable events: verify scheduler stacks and graphics/compositor events in the resulting
trace.

Collector stop always targets its own generated instance, including on failures. A failed
start never triggers a stop. Failed shutdown records the exact instance-specific recovery
command; never substitute an unqualified stop/cancel. App timeout requests a normal close
on **only its own process**; if it remains hung, it is left running and the run is incomplete.
The timer bounds collection even if the app hangs. A crash, launch failure, missing anchors,
changed snapshot, non-overlapping timestamps or failed WPR command prevents a clean Check.

Check validates conservative full-process overlap and the app's startup/shutdown QPC anchors,
then runs `xperf -a tracestats`, retaining output, decoder warnings and exit status. It does
**not** use `-tle` or claim zero loss from successful process exit. Inspect
`trace-statistics.txt` and WPR logs for event/buffer loss and decoder diagnostics (#873);
`check.json` explicitly leaves this review pending. Missing xperf or failed decoding is an
error, not an empty successful report. Use WPA CPU Usage (Precise) on the recorded PID/UI
TID and marked counter intervals to distinguish running, waiting and runnable delay, then
correlate GPU/DWM events. Sampled CPU totals alone do not explain a two-second wall stall.

Repeat the same workload across multiple captures on the affected hardware. Record max
and counts strictly above 100/250 ms alongside rolling p99; absence of a reproduced freeze
in one run is not a root-cause fix. Script lifecycle/error tests run via
`ctest --preset win-debug -R "ResizeCaptureScript|EtwCaptureScript|EtwAnalysisScript"` when
PowerShell 7 is available, or `pwsh -File tools\test-profile-etw-resize.ps1`,
`pwsh -File tools\test-profile-etw.ps1` and `pwsh -File tools\test-analyze-etw.ps1`. The
capture tests mock WPR and never start a recording; the analysis tests use canned xperf output.

### Windows — Present-interval capture (PresentMon)

TaskSmack's own frame figures -- the Render Metrics overlay (**Ctrl+Shift+M**) and the `ResizePerf`
`frame`/`loop` lines -- are measured inside the app and end when its buffer swap returns. They
cannot show whether, or when, a frame reached the screen. [PresentMon](https://github.com/GameTechDev/PresentMon)
reads the presents Windows itself records (through ETW) and when each one was displayed, so
judging frame pacing honestly (#843, Phase 0) needs its numbers next to the app's own.

**Getting it.** Download the console build, `PresentMon-<version>-x64.exe`, from the
[releases page](https://github.com/GameTechDev/PresentMon/releases): a single executable with
nothing to install (the `.msi` there installs the GUI overlay application instead). The commands
below use the options of the 2.x console application
([README-ConsoleApplication.md](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)).
PresentMon runs an ETW session, so it needs an elevated terminal, or an account in the
**Performance Log Users** group (otherwise it fails with "failed to start trace session (access
denied)"). Elevating PresentMon does not elevate TaskSmack: PresentMon only observes, so start
TaskSmack as usual from a normal terminal (an elevated target skews what is measured, #872).

```powershell
# TaskSmack already running from a normal terminal. Elevated terminal, repository root:
# one 60 s capture of TaskSmack's presents to a CSV, then exit.
.\PresentMon-2.6.0-x64.exe --process_name TaskSmack.exe --output_file perf-data\presentmon-idle.csv `
    --timed 60 --terminate_after_timed --session_name TaskSmackPresentMon

# A leftover session of that name (e.g. after a killed capture): stop it, then exit.
.\PresentMon-2.6.0-x64.exe --session_name TaskSmackPresentMon --terminate_existing_session
```

- `--process_name` records only that executable's presents; `--output_file` writes the CSV (by
  default `PresentMon-<time>.csv` in the current directory); `--timed` stops recording after that
  many seconds and `--terminate_after_timed` then exits. `--delay <s>` waits before recording.
- `--session_name` gives the capture its own ETW session name, so it never collides with another
  PresentMon. As with the WPR captures, never use `--stop_existing_session` on a session that is
  not yours.
- **Idle:** leave TaskSmack on the tab being measured and do not touch it for the whole window.
  **Interactive:** start the capture, then move the pointer over the window, scroll or resize for
  the whole window. Capture each scenario separately, at the same window size and display, and
  name the CSV after it.
- Leave the Render Metrics overlay off for the capture that counts: while it is open TaskSmack
  draws and measures more. Turn it on only to compare its readout with PresentMon's.

**Columns that matter** (all times in ms; the default 2.x CSV, one row per present):

| Column | Meaning | Compare with |
|---|---|---|
| `MsBetweenPresents` | Time from the previous present to this one: the cadence TaskSmack presents at | `ResizePerf` `loop` (deliver-to-deliver); the overlay's FPS as 1000 / mean |
| `MsBetweenDisplayChange` | How long the previous frame stayed on screen before this one replaced it: the cadence the user sees. `NA` if this frame was never displayed | Should track `MsBetweenPresents`; with vsync it comes in whole display refreshes (16.7 ms at 60 Hz) |
| `MsUntilDisplayed` | From the present call until the frame was displayed. `NA` means the frame was never displayed (dropped): count these | -- |
| `MsInPresentAPI` | Time spent inside the present call | `ResizePerf` `swap` |
| `PresentMode` | How the frame reached the screen (e.g. `Composed: Flip`, `Hardware: Independent Flip`) | Compare only captures with the same mode: it changes latency and pacing |
| `TimeInSeconds` | Time of the present since recording started (`TimeInQPC`, a raw performance-counter value, with `--qpc_time`) | Lines a stall up with the app log or an ETW trace |

TaskSmack renders with OpenGL, which PresentMon instruments less fully than Direct3D (it typically
reports such apps' runtime as `Other`), so read the present and display intervals above and treat
its CPU/GPU busy and latency columns with caution. `--v1_metrics` writes the PresentMon 1.x layout
instead (lower-case `msBetweenPresents`/`msBetweenDisplayChange`, plus a `Dropped` column).

Percentiles, nearest rank as in the `ResizePerf` lines:

```powershell
$rows = @(Import-Csv perf-data\presentmon-idle.csv)
function Get-Percentile([double[]]$Sorted, [double]$P) {
    $Sorted[[math]::Max(0, [math]::Ceiling($P / 100 * $Sorted.Count) - 1)]
}
if ($rows.Count -eq 0) { 'No presents recorded: is TaskSmack running, and is --process_name right?' }
foreach ($column in 'MsBetweenPresents', 'MsBetweenDisplayChange') {
    $values = @($rows | Where-Object { $_.$column -and $_.$column -ne 'NA' } | ForEach-Object { [double]$_.$column } | Sort-Object)
    if ($values.Count -eq 0) { "${column}: no values (no presents, or no displayed frames)"; continue }
    '{0}: n={1} p50={2:N2} p95={3:N2} p99={4:N2} max={5:N2} ms' -f $column, $values.Count,
        (Get-Percentile $values 50), (Get-Percentile $values 95), (Get-Percentile $values 99), $values[-1]
}
'Not displayed: {0} of {1} presents' -f @($rows | Where-Object MsUntilDisplayed -eq 'NA').Count, $rows.Count
```

**Reading them against the app's own metrics.** The overlay's `Frame: <ms> (<fps> FPS)` line is
ImGui's rolling average over the last 60 frames, and the status bar's FPS readout an average over
half a second. Over a steady stretch, 1000 / mean `MsBetweenPresents` should agree with them; if it
does not, the app is not presenting every frame it counts. Expected cadences are those in the FAQ
and above: near 50 ms at idle (the idle period, or a chart's animation period while it moves), a
whole number of refreshes near 16.7 ms while interacting on a 60 Hz display, and about 200 ms
minimized. For tails, set `TASKSMACK_TRACE_RESIZE_PERF=1` during the capture and compare the
`loop` p95/p99 with `MsBetweenPresents`' p95/p99. A `MsBetweenDisplayChange` tail, or `NA` rows,
where `MsBetweenPresents` is even means the frames were produced on time but reached the screen
late or not at all, which no in-app counter can show.

**Relation to the ETW captures.** PresentMon answers *whether* frames reach the screen evenly;
`tools/profile-etw.ps1` answers *why* they do not. PresentMon is itself an ETW consumer of the
graphics providers (DXGI, DxgKrnl, DWM) that the `resize` focused profile also records, in a
session of its own. To find a PresentMon stall in a WPR trace recorded over the same period, add
`--qpc_time`: the CSV then carries each present's raw performance-counter value, the timebase ETW
traces and the `ResizePerfAnchor` lines use. Use PresentMon first to find and size a pacing problem
cheaply, then a WPR capture to attribute it.

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

TaskSmack has built-in frame-timing instrumentation that works in any build, covering both
interactive (resize/move) and idle/steady-state frames (perf-plan #843 phase 0 — idle-time
performance is priority 1 for the app, so idle frames get timed too, not just resize gestures).
Despite the env var's resize-focused name (it started as a resize-specific investigation), it now
runs continuously whenever enabled. Enable it by setting `TASKSMACK_TRACE_RESIZE_PERF=1` before
launching:

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

Just let the app sit idle for a few seconds to capture steady-state numbers, and/or resize the
window (edges and corners) for 20–30 seconds to capture interactive numbers, then close the app.
The log contains `ResizePerf[idle-progress|idle-end|interaction-progress|interaction-end|shutdown]` lines.
Idle frames log every 5 s (`IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS`); interaction frames log every
0.5 s (`RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS`) since an interaction is a short, bounded burst
where more frequent logging is useful. Each line reports avg/p95/p99/max per phase, not just
avg/max — p95/p99 answer "did we miss the 16.6 ms (60 fps) frame budget at the tail", which an
average can hide and a single max spike can overstate:

```
ResizePerf[idle-progress]: batches=94 events=6 resizeEvents=0 maxBatchEvents=2
  frames=94 resizeFrames=0
  frame avg/p95/p99/max=4.421/6.912/7.340/7.580 ms       ← update+render+post+swap (the 16.6ms/60fps figure)
  loopIntervals=94 loop avg/p95/p99/max=50.120/51.034/52.880/53.410 ms ← frame end to frame end (cadence)
  drain avg/p95/p99/max=0.031/0.084/0.121/0.121 ms      ← SDL event drain
  update avg/p95/p99/max=0.014/0.031/0.045/0.052 ms     ← domain model refresh (all layers)
  render avg/p95/p99/max=0.842/1.203/1.410/1.502 ms     ← ImGui layout + draw call generation
  post avg/p95/p99/max=0.611/0.798/0.850/0.902 ms       ← post-render (all layers)
  swap avg/p95/p99/max=2.940/4.812/5.220/5.401 ms       ← GL buffer swap (includes vsync stall)

ResizePerf[interaction-progress]: batches=109 events=48 resizeEvents=36 maxBatchEvents=4
  frames=109 resizeFrames=109
  frame avg/p95/p99/max=4.447/17.462/19.960/22.443 ms
  loopIntervals=109 loop avg/p95/p99/max=16.702/17.910/20.330/23.020 ms
  drain avg/p95/p99/max=0.140/1.802/2.101/2.278 ms
  update avg/p95/p99/max=0.018/0.940/1.220/1.453 ms
  render avg/p95/p99/max=0.572/6.310/7.980/8.798 ms
  post avg/p95/p99/max=0.558/0.712/0.760/0.784 ms
  swap avg/p95/p99/max=3.299/9.510/11.200/12.408 ms
```

`frame` is update+render+post+swap (drain is a separate per-loop-iteration accumulator, not
always 1:1 with a rendered frame, so it's reported separately) — this is what #843's success
criterion "p99 frame time ≤ 16.6ms (60fps)" actually refers to: individual phase percentiles can
each look fine on their own while their sum still misses the frame budget.

`loop` is the deliver-to-deliver interval: wall time from one presented frame's end (after swap)
to the next one's, so it includes the pacing wait, the event drain and any skipped render in
between. A render skipped for a drain overrun (`skippedFrames=`) is not an interval of its own; its
time stays in the interval that spans it. `frame` says how long the work took; `loop` says how
evenly frames reached the screen (at idle it should sit near the 50 ms idle period, or the
animation period while a chart moves).

`frames=`/`batches=` count only the current interval (5s idle / 0.5s interaction), but the
p95/p99 figures are computed over a rolling window of up to 200 samples that persists across
consecutive same-state intervals (only cleared when idle and interaction actually transition,
so idle and interaction data never mix) — see `PERCENTILE_WINDOW_SIZE` in `ResizePerfTrace.h`.
That's why, e.g., a `frames=94` idle-progress line above can still show p99 below max: with
nearest-rank percentiles, p99 is mathematically forced to equal max whenever the *window* it's
computed over holds fewer than 100 samples, which a single 94-frame interval alone would — the
rolling window carrying samples over from prior intervals in the same state is what avoids that
degenerate case in steady, continuous idle/interaction periods.

(Figures above are illustrative shapes, not a specific captured run — always compare against a
fresh capture on your own machine, not these numbers.)

`over100`/`over250` count frame phase sums strictly above those thresholds in the current
interval. Partial idle intervals are flushed at interaction start instead of discarded.
Frames/layers at or above 100 ms emit `ResizePerfSlowFrame`/`ResizePerfSlowLayer`;
`ResizePerfTitleBarSlowUpdate` retains its 250 ms threshold. Routine budget misses stay in
periodic summaries instead of producing synchronous per-frame log spam.

`ResizePerfOperation` isolates regular and final mouse-release SDL size calls, wrapper
size/sync calls, viewport updates, swap-interval changes, ImGui CPU finalization and
OpenGL backend submission. Detail is emitted only on SDL failure or durations above 100 ms,
with begin/end performance counters and requested geometry (`requestedA/B`: logical
width/height for size, pixel width/height for viewport/submission, interval/0 for vsync).
SDL result/error is preserved; void GL/ImGui operations explicitly report
`result=not-queried`, not GPU success. `ResizePerfOperations` shutdown lines retain run-wide
count/max/over100/over250/failures and the requested values of the longest call. There are
no `glFinish`, `glGetError`, or synchronous GPU timing queries; backend submission wall
time includes its texture/state/buffer/callback work and possible driver waits.

`ResizePerfLoop` records actual loop-boundary wall time, drain, wait, vsync transition and
frame phase sum for loops above 100 ms. `other` is the residual (logging, instrumentation,
bookkeeping and scheduling outside measured phases); it is not assumed to be CPU work.
The loop closes at the next loop boundary, so end-of-loop summary logging is included.
`ResizePerfFrameGap` measures consecutive swap completions, including intervening sleeps
and skipped-render loops; the first frame has no fabricated gap. These measures include
intentional 50/200 ms idle/minimized waits, unlike the 16.6 ms phase-sum target.
`ResizePerfWallSummary` retains their run-wide maxima and threshold counts.

Startup/shutdown `ResizePerfAnchor` records PID/UI TID, full configure-time build identity,
UTC Unix nanoseconds bracketed by performance-counter samples, and counter frequency.
On Windows the SDL counter is raw QPC; begin/end counter ranges can be aligned directly
with ETW's QPC timebase. The bracket exposes sampling uncertainty, and two anchors expose
wall-clock changes. Human-readable spdlog timestamps are local time and may be delayed by
logging; use the numeric anchors, not timestamps from a different capture.

You can also control the spdlog runtime level directly (useful for CI or scripted runs). This
only changes which already-emitted log messages are visible — it does **not** enable ResizePerf
frame-timing collection by itself; that still requires `TASKSMACK_TRACE_RESIZE_PERF=1` regardless
of log level, since `Application` only enables tracing from that env var:

```bash
# Raise the default log verbosity in a release build. Does NOT enable ResizePerf tracing on
# its own -- combine with TASKSMACK_TRACE_RESIZE_PERF=1 above to see ResizePerf lines.
TASKSMACK_LOG_LEVEL=info ./build/optimized/bin/TaskSmack

# Full debug verbosity in an optimized build
TASKSMACK_LOG_LEVEL=debug ./build/optimized/bin/TaskSmack
```

`TASKSMACK_LOG_LEVEL` accepts any spdlog level name: `trace`, `debug`, `info`, `warn`,
`error`, `critical`, `off`. When both env vars are set, `TASKSMACK_LOG_LEVEL` takes
precedence.

### Measuring idle CPU and frame time

Idle cost is TaskSmack's first performance priority (#843): a task manager that sits open all day
must not be the thing using the CPU. `tools/measure-idle.sh` (Linux) turns one idle scenario into a
comparable set of numbers. It launches TaskSmack with `TASKSMACK_TRACE_RESIZE_PERF=1`, waits for the
main loop and a warm-up (as `tools/profile-perf.sh app` does), samples per-thread CPU, closes the app
with SIGTERM (a non-zero exit or a SIGKILL fails the run), and prints a table plus one
machine-readable `RESULT` line. With `--repeat N` it runs that whole cycle N times (a fresh process
each time) and adds the mean, median and p95 of app CPU, total CPU and fps across the repetitions,
plus a `SUMMARY` line. Every run also writes JSON (`perf-data/idle-<label>-<timestamp>.json`, or
`--json <path>`) with each repetition's figures and per-thread rows, the aggregates, and provenance:
commit and dirty flag, preset and build type, refresh interval and history window (as the app logged
or loaded them), synthetic spec, the window's measured geometry and any `--window` requested, GL
renderer, display refresh rate, CPU model, logical CPU count, MHz, and whether it ran under WSL. `--fail-above <pct>` exits with status 3 when the median app CPU
is above `<pct>`. Another TaskSmack running with the same settings fails the run (the
single-instance lock would stop the new one at its "already running" box); close it first.

```bash
# Default: profile preset (built first), 15 s warm-up (45 s for a "minimized" label), 30 s sample
./tools/measure-idle.sh --label overview

# Existing debug build; switch tabs before the warm-up with any command (it gets TASKSMACK_PID).
# The click position is the tab's screen position, which depends on your window size and place.
./tools/measure-idle.sh --preset debug --skip-build --label processes \
    --setup-cmd 'sleep 2; xdotool mousemove <x> <y> click 1'

# Gate: the fixed 1600x900 window, five repetitions, fail if the median app CPU is above the
# target below. --window replaces the saved geometry for the run without changing it (#1453).
./tools/measure-idle.sh --skip-build --window 1600x900 --repeat 5 --fail-above 13
./tools/measure-idle.sh --skip-build --window 1600x900 --repeat 5 --fail-above 14 \
    --synthetic processes=5000,history=full

./tools/measure-idle.sh --help
```

Per-thread CPU comes from `pidstat -u -t -p <pid> 1 <N>` (package `sysstat`) when installed,
otherwise from `/proc/<pid>/task/*/stat` deltas over the same window. Threads are named so the rows
are readable: the background samplers are `ts-sampler-proc` (process enumeration) and
`ts-sampler-sys` (system/storage/GPU), plus `ts-sampler-svc` (the Services tab's list, created
the first time that tab is shown). On Linux the UI thread keeps the process name (`TaskSmack`;
its TID equals the PID), because renaming the main thread renames the process for `ps`, `top` and
`pgrep`. On Windows the UI thread is described as `tasksmack-ui` and shows in WPA and debuggers.
Names come from `Platform/ThreadName.h`; give any new worker thread one there (15 bytes at most).

**Metrics:**

| Metric | Definition | Source |
|---|---|---|
| Thread CPU% | Average CPU time of one thread over the sample window; 100% = one logical CPU busy | pidstat / `/proc` |
| Total CPU% | The whole process over the window, including threads that started or exited in it | pidstat / `/proc/<pid>/stat` |
| App CPU% | Total CPU% minus Mesa's software-render/driver threads (`llvmpipe-N`, `<proc>:disk$N`; the list is in the script's header). The figure the idle target applies to | pidstat / `/proc` |
| fps | Presented frames per second: loop intervals ÷ their summed duration | `ResizePerf[...]` `loop` |
| Frame p95/p99/max | update+render+post+swap per presented frame (the 16.6 ms budget figure) | `ResizePerf[...]` `frame` |
| Loop p95/p99 | Deliver-to-deliver interval, frame end to frame end, skipped renders included | `ResizePerf[...]` `loop` |

The frame figures come from the `ResizePerf` summaries TaskSmack logs on its own schedule (every 5 s
at idle), so they cannot cover exactly the CPU sample. The script uses the summaries logged while it
sampled CPU and prints the span they actually cover (from the summary before the first one to the
last one) next to the CPU sample's start and end. The `RESULT` line carries both as `cpuStart`/`cpuEnd`
and `traceStart`/`traceEnd`/`traceSpan`. The two spans differ by up to one summary interval at each
end. The p95/p99 figures are the worst of those summaries (each is nearest-rank over a rolling window
of up to 200 samples); max is the largest per-interval max among them.

The warm-up keeps startup and tab-switch frames out of that 200-sample rolling window before
sampling starts. The default is 15 s, enough at idle frame rates (20–60 fps). A minimized window
presents only about 5 fps (`MINIMIZED_FRAME_SLEEP_MS = 200`), so it needs about 45 s to refill the
window; with fewer than 100 samples, nearest-rank p99 also just equals the max. The script uses 45 s
by default when `--label` contains `minimized`, and `--warmup` overrides either default.

**Scenario matrix:** run each for 30 s at the default 1 s refresh, window left alone, after a
warm-up on that tab:

| Scenario | What it exercises |
|---|---|
| Overview | System charts: CPU, memory, battery, threads/faults |
| Processes | The process table and process enumeration |
| CPU Cores | One chart per logical CPU |
| Minimized (optional) | The hidden-window pacing path; should be close to the sampler threads alone. Use `--label minimized` (45 s warm-up) and minimize the window in `--setup-cmd` |

Compare like with like: the same machine, preset, window size and refresh interval, and the same
scenario. Run each scenario more than once; one run on a shared desktop is noisy.

**WSL:** by maintainer decision, numbers measured under WSL (WSLg) are **CPU-only evidence**. CPU%
of TaskSmack's own threads is meaningful there, but WSLg usually renders through Mesa's `llvmpipe`
software rasterizer, whose threads (`llvmpipe-N`) then dominate the total and make frame and loop
times reflect the CPU rasterizer and the shared desktop, not a GPU driver and compositor. Quote
fps/frame/loop figures only from native Linux or Windows; on WSL quote CPU% (and say so).
On WSL that
means app CPU: total CPU there is mostly `llvmpipe` (about 490–550% of one CPU in the baseline
below) and says nothing about TaskSmack.

#### Idle-CPU target (Linux/WSL app CPU)

Per maintainer decision D1 (#1408, part of #843: measure first, then set a target), the idle-CPU
target is, at a fixed **1600×900 window, not maximized** (`--window 1600x900`):

| Scenario | Target: median app CPU over `--repeat 5` |
|---|---|
| Default (Overview tab) | **≤ 13%** of one logical CPU (`--fail-above 13`) |
| `--synthetic processes=5000,history=full` | **≤ 14%** of one logical CPU (`--fail-above 14`) |

Both targets are about 1.5× the measured median on a quiet machine, rounded up: 8.08% → 13%, and
9.02% → 14%. That leaves room for run-to-run noise (one default repetition read 15.0% against a
median of 8.1%; the median over five absorbs a single outlier) while still catching a regression
that adds a few percent of one core at idle. A median above the target is a regression to explain
or fix. The targets cover **Linux/WSL app CPU only**; Windows targets will follow from
`tools/measure-idle.ps1` captures. Re-measure and revisit them when the renderer, the default
scenario or the sampling defaults change.

Measure at the targets' window geometry. TaskSmack restores its saved size and maximized state, and
a larger window renders more, so a run without `--window` measures whatever the machine last saved
and is not comparable with the targets. The script records the window's size and maximized state in
the `SUMMARY` line and the JSON (`scenario.window`), and warns when it varied between repetitions.

`--window <width>x<height>[,maximized]` (#1453) launches TaskSmack with `TASKSMACK_WINDOW=<value>`,
which opens the window at that size (window units, clamped to 200–16384 and to the display),
maximized only when the value says so, instead of the saved geometry. While the variable is set
TaskSmack logs `TASKSMACK_WINDOW is set: 1600x900, not maximized; window geometry will not be saved`
and does not write its `[window]` geometry on exit, so a measurement never changes your saved
window; other settings save as usual. An invalid value is ignored with a warning (the script rejects
one before launching). The JSON records the request as `scenario.requestedWindow` (`spec`, `input`,
`width`, `height`, `maximized`, and `applied`: whether every repetition's app log shows the
override, which a binary built before #1453 would not) next to the measured `scenario.window`.
`--window` stays opt-in for ad-hoc runs; the gate commands above pass it.

Measure on a quiet machine. App CPU rises with presented frames, and other load slows `llvmpipe`
and so the frame rate. A first baseline taken under load average 20–30 read about 40% lower than
the same build measured quiet, so a loaded run can pass a target it would fail when quiet.

Baseline (2026-10-07, binary built from `43fade9a`, measured with the script at `c49965f1`,
`profile` preset = RelWithDebInfo, `/proc` sampler, 15 s warm-up, 30 s samples, 5 repetitions each,
load average under 1.5 before the runs, `--window 1600x900`). Intel Core Ultra 7 255H, 10 logical
CPUs, WSL2 (kernel 6.18), WSLg with Mesa 26 `llvmpipe` (LLVM 21), 59.98 Hz display. Measured with
the maintainer's config: **250 ms refresh** and a **300 s history** (the synthetic run preloads
1800 s; the defaults are 1000 ms and 300 s). A 250 ms refresh is the heavier case.

| Scenario (1600×900) | App CPU% mean / median / p95 | Total CPU% mean / median / p95 | fps mean / median |
|---|---|---|---|
| Default | 9.85 / 8.08 / 14.97 | 114.94 / 115.94 / 128.12 | 43.67 / 43.27 |
| `processes=5000,history=full` | 9.06 / 9.02 / 9.33 | 74.46 / 74.41 / 76.79 | 20.87 / 20.80 |

p95 over five repetitions is their maximum (nearest rank). For reference, the first quiet baseline
(binary `f4cf041a`) ran maximized at 3840×2100 and read 10.60% / 14.70% median app CPU at
24 / 22 fps: a larger window renders more, which is why the targets now name their geometry.

#### Synthetic large-UI scenario (captures at the limits)

The scenarios above measure whatever machine you happen to be on. To measure the UI at its limits --
thousands of processes, many cores, disks and interfaces, and every chart holding the longest history
(30 minutes at 100 ms, 18k samples per series) from the first frame -- run TaskSmack against a
synthetic machine instead (#1413). Set `TASKSMACK_SYNTHETIC`, or pass `--synthetic` to the script:

```bash
# Overview at the limits: 5000 processes, every history chart full from the first frame
./tools/measure-idle.sh --preset profile --label synthetic-overview --synthetic processes=5000,history=full

# The process table at 2000 processes (switch tabs with --setup-cmd as above)
./tools/measure-idle.sh --skip-build --label synthetic-processes --synthetic processes=2000 \
    --setup-cmd 'sleep 2; xdotool mousemove <x> <y> click 1'

# Or run the app directly
TASKSMACK_SYNTHETIC=processes=2000,cores=64,history=full ./build/debug/bin/TaskSmack
```

`TASKSMACK_SYNTHETIC` takes comma-separated `key=value` settings (a bare `1` takes every default):

| Key | Default | Meaning |
|---|---|---|
| `processes` | 2000 | Processes, in a realistic tree (kernel threads, daemons, a desktop session, a browser, an editor with language servers, containers, terminals with builds) with plausible, slowly varying CPU, memory, I/O and network; build jobs and some browser tabs come and go |
| `cores` | 16 | Logical CPUs (one CPU Cores chart each) |
| `disks` | 4 | Disks |
| `interfaces` | 4 | Network interfaces (two physical, the rest virtual) |
| `seed` | 1413 | Generator seed: the same seed gives the same machine on every platform |
| `history` | `full` | History preloaded at startup: `full` (`HISTORY_SECONDS_MAX` at `REFRESH_INTERVAL_MIN_MS`), `none`, or a number of seconds |
| `refresh` | the configured one | Refresh interval (ms) to start at |

What it changes, and what it doesn't:

- It is opt-in and read once at startup. Unset (or `0`/`off`), TaskSmack builds exactly the probes it
  always does; the only difference is one `getenv` at startup.
- The App composition root builds the models on `Platform::Synthetic` probes (`src/Platform/Synthetic/`)
  instead of the real ones. Every counter is a closed-form function of time, so the live probes and the
  preload agree and successive samples give consistent deltas.
- The history preload fills SystemModel, StorageModel and ProcessModel's system histories through their
  series APIs (one publish for the whole window). Process Details' per-process history still starts when
  you select a process.
- The history window and refresh overrides apply to the run only; `config.toml` is not changed (the
  Settings dialog still shows the configured values).
- There is no synthetic GPU or battery: those sections show their empty states. Every process action
  (end, kill, suspend, priority) is refused, since synthetic PIDs may be real ones.
- A warning is logged at startup (`TASKSMACK_SYNTHETIC is set: showing a synthetic machine...`), and the
  host name reads `tasksmack-synthetic`.

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

Use CMake’s `FetchContent` for dependencies. Declare new dependencies in `cmake/Dependencies.cmake`, always use `SYSTEM` to suppress third-party warnings, and always pass a per-preset `BINARY_DIR` (see "Shared FetchContent cache" below):

```cmake
FetchContent_Declare(
    mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/mylib-build"
)
FetchContent_MakeAvailable(mylib)

target_link_libraries(TaskSmack PRIVATE mylib)
```

### Shared FetchContent cache

The shared FetchContent cache is **enabled by default** to reuse downloads across presets, reducing build times and bandwidth usage. The cache is stored at `.cache/fetchcontent/` in the project root.

Only the downloads are shared: `.cache/fetchcontent/` holds each dependency's `<dep>-src` and
`<dep>-subbuild`, while its `<dep>-build` tree lives in the preset's own `build/<preset>/_deps/`, so
a sanitizer, coverage or LTO preset never reuses objects another preset compiled with different
flags (#1308). Configuring fails if any added directory builds outside the preset's build tree --
that is the check that a new `FetchContent_Declare()` without `BINARY_DIR` trips.

To disable the cache:

```bash
cmake --preset debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
cmake --preset win-debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
```

Override the cache dir with `TASKSMACK_FETCHCONTENT_CACHE_DIR` or `FETCHCONTENT_BASE_DIR`.

## CI/CD

We use GitHub Actions for our CI workflows. They are categorized as follows:

### Core Build & Test
- **`ci.yml`**: The primary hub. Runs on pushes to `main`, PRs to `main`, nightly (Monday to Saturday; the Sunday weekly run is the same full run plus the unity build), and via manual dispatch. It detects docs-only pull requests to skip C++ builds and `clang-tidy`. It runs Linux and Windows Debug builds on push/PR, a Linux Release build on the same events plus the nightly schedule (Windows Release runs nightly and on dispatch only; the unity build weekly), compiles and links (but does not run) `TaskSmackBenchmarks` in that Linux Release job so a PR that breaks the benchmark build fails CI (#1348), checks markdown links, runs `clang-tidy` (blocking) on Linux and on Windows on PRs/schedule/dispatch (skipped on docs-only PRs and on pushes to `main`; the Windows job is also skipped when every change is Linux-only). On a PR it analyzes only the translation units the change can affect -- changed `src/` `.cpp` files plus every `.cpp` that includes a changed `src/` header -- and falls back to every file when `.clang-tidy`, the tidy scripts, CMake files, `ci.yml`, the toolchain setup actions (`setup-llvm`, `setup-windows-llvm`, `setup-python-glad`) or `requirements-glad.*` change (`tools/tidy-changed-files.py`, #1406); the nightly run analyzes every file, runs IWYU (include analysis) only via manual dispatch, and runs a **blocking** Address/Undefined Behavior sanitizer job (`ASan+UBSan`, part of `CI Success`) on PRs. It outputs a `CI Success` gate job used for branch protection; when a needed job was cancelled (a superseded push or a manual stop) the gate still fails, but its first step reports "CANCELLED, not a code failure" so it isn't mistaken for a broken build.
- **`reusable-build-test.yml`**: Contains the actual matrix steps for setting up LLVM, Python, `ccache`, configuring CMake, building, and running CTest tests, plus an optional Linux build-only `TaskSmackBenchmarks` step (`build_benchmarks` input). Called by other workflows. It configures with `-DTASKSMACK_ENABLE_PCH=OFF` (except the weekly unity build) because ccache can't cache PCH-using compiles; local presets keep PCH on.
- **`manual-build.yml`**: Manual dispatch entry point to trigger a specific OS and build type build from the GitHub UI without opening a PR.

CI caches (#1406): ccache and the FetchContent source cache (`.github/actions/fetchcontent-cache`) are **saved only by runs on `main`** in every workflow, `release.yml` included (tag runs only restore); pull requests restore main's entries and never save their own, which kept the repository under its 10 GB Actions cache quota. One job per OS saves the FetchContent cache: Windows debug, and the Linux release build after its benchmark-enabled configure, so the saved sources include Google Benchmark. The FetchContent key hashes only the files that declare or patch dependencies (`cmake/Dependencies.cmake`, `cmake/patches/**`, `tests/CMakeLists.txt`, `benchmarks/CMakeLists.txt`). The `clang-tidy` jobs don't use ccache (clang-tidy doesn't compile through it).

### Security & Fuzzing
- **`codeql.yml`**: Runs GitHub's CodeQL engine to trace execution and analyze the C/C++ codebase for semantic security vulnerabilities (pushes/PRs to main, weekly).
- **`osv-scanner.yml`**: Uses Google's OSV-Scanner to check dependencies against the Open Source Vulnerability database (pushes to main, weekly, manual dispatch).
- **`renovate.yml`**: Self-hosted [Renovate](https://docs.renovatebot.com/) run, scoped to C++ `FetchContent` libraries and the build/dev toolchain (LLVM, Python, CMake, Ninja, ccache, pre-commit's own hook tools) -- the freshness gap Dependabot/OSV-Scanner don't cover (weekly, manual dispatch with dry-run options). See "Keeping Dependencies Current" below.
- **`scorecard.yml`**: Evaluates the repository against OpenSSF security best practices (branch protection, pinned dependencies) and uploads results to the security dashboard (weekly, on branch-protection changes, and manual dispatch). Its SAST check counts a merged PR as scanned only if a code-scanning check run (GitHub Advanced Security's `CodeQL` or `osv-scanner`) has completed on the PR's head commit when Scorecard runs. It used to run on every push to `main`, seconds after the merge, which scored a PR merged before its CodeQL finished as unscanned (#1405); the weekly run sees those results long after they land (#1406).
- **`dependency-review.yml`**: Scans PRs to block any that introduce vulnerable dependencies (CVE-based) in package manifests/lockfiles.
- **`sanitizers.yml`**: Performs heavy blocking runs using Address/Undefined Behavior (ASan+UBSan) and Thread (TSan) sanitizers, generating HTML reports of memory leaks or data races. A push to `main` runs TSan only, because ASan+UBSan already gates every PR in `ci.yml`; manual dispatch runs both, and `heavy-checks.yml` runs both nightly.
- **`main-health.yml`**: After every run of the main workflows on `main` (CI -- which includes the nightly full `clang-tidy` --, CodeQL, Sanitizers, Heavy Checks, OSV, Pre-commit; not the dispatch-only `static-analysis.yml`), opens or updates a single tracking issue labelled `ci-red-main` while any of them is red, and closes it when all are green again (#1406).
- **ClusterFuzzLite (`cflite_*.yml`)**: Google's continuous fuzzing suite. Runs on PRs (`cflite_pr.yml`), pushes to main (`cflite_build.yml`), and weekly for batching and pruning corpora (`cflite_batch.yml`, `cflite_prune.yml`).

### Code Quality & Hygiene
- **`pre-commit.yml`**: Runs the `pre-commit` framework (via Python) across all files to enforce syntax hygiene, formatting, and file-level rules configured in `.pre-commit-config.yaml` (pushes to main, PRs).
- **`static-analysis.yml`**: Manual full `clang-tidy` run on Linux and on Windows, both blocking (manual dispatch only; `ci.yml`'s nightly schedule runs the full analysis on `main`).
- **`heavy-checks.yml`**: Runs expensive verifications that shouldn't block PR feedback loops, such as generating Coverage reports (nightly, manual dispatch, and pushes to `main` that touch code the benchmarks measure -- `src/Domain`, `src/UI`, `src/Platform`, `ProcessDetailsHistory` -- or the benchmarks, their baseline and tools). The coverage jobs report **line** coverage from `coverage/coverage.lcov` and warn (never fail) when it drops below a floor set a few points under the current baseline; `codecov.yml`'s `auto` target is the per-change ratchet (#1543). A manual dispatch with `scope: benchmark` runs only the benchmark-regression job. The Linux coverage job also runs `tools/check-prereqs.sh` on a fresh image.

### Release & Operations
- **`release.yml`**: Handles compiling production binaries, packaging them (ZIP/tarballs, deb), and publishing GitHub Releases on `v*.*.*` tags.
- **`changelog.yml`**: Automates changelog generation using `git-cliff` for strict `vMAJOR.MINOR.PATCH` tags.
- **`pr-labeler.yml`**: Automatically assigns labels (e.g., `bug`, `enhancement`, `docs`) to pull requests based on `.github/labeler.yml` file globs.
- **`copilot-setup-steps.yml`**: Bootstraps the repository environment (CMake, LLVM, etc.) for GitHub Copilot cloud agent sessions.

PR optimization: docs-only pull requests skip compile/test and `clang-tidy` jobs in `ci.yml` to keep feedback fast.

Concurrency: a new push to a PR branch cancels that branch's in-progress runs, but pushes to `main` never cancel
each other. `ci.yml`, `sanitizers.yml` and `heavy-checks.yml` give each `main` commit its
own concurrency group, so back-to-back merges each get a complete run and a regression is blamed on the commit
that caused it (#1187). `codeql.yml` doesn't cancel `main` runs either, but queues them in one group.
`pre-commit.yml` and `dependency-review.yml` also cancel a PR's superseded run.

Dependabot updates GitHub Actions and Python dependencies weekly.
[OSV Scanner](https://google.github.io/osv-scanner/) scans C++ FetchContent dependencies
(via Syft SBOM generated from `CMakeLists.txt`) and all four Python dependency manifests against the
[OSV vulnerability database](https://osv.dev) on pushes to `main` and the weekly scheduled run. Results appear in the repository's **Security → Code scanning** tab.
Release and CI builds install GLAD's Python dependencies from the hash-locked
`requirements-glad.txt`; the LLVM bootstrap action also verifies the downloaded installer's
SHA-256 digest before executing it.

### Keeping Dependencies Current

Three automated mechanisms cover different parts of "is this pinned version stale/vulnerable,"
and one category is deliberately left manual. See #89 for the history.

**One-time setup for `renovate.yml`** (two steps, neither done automatically since both are
account/repo-level security settings):

1. **Required**, or the workflow can't open PRs at all: enable Settings → Actions → General →
   Workflow permissions → "Allow GitHub Actions to create and approve pull requests". Verify it's
   on via `gh api repos/mgradwohl/tasksmack/actions/permissions/workflow` --
   `can_approve_pull_request_reviews` must be `true`; `renovate.yml` fails on every run while
   it's `false`.
2. **Required**, or `renovate.yml` fails immediately with an actionable error instead of running:
   PRs opened with the default `GITHUB_TOKEN` don't trigger other workflows (`ci.yml` included)
   -- GitHub's loop-prevention behavior for the built-in token. `renovate.yml` deliberately does
   not fall back to `GITHUB_TOKEN` (it used to, silently, which let dependency-update PRs skip CI
   and be merged unvalidated); it fails the run instead until this is set up. Add a fine-grained
   PAT scoped to just this repo, with Contents, Pull requests, Issues (needed because
   `dependencyDashboard: true` in `renovate.json5` has Renovate create/update a tracking issue),
   and **Workflows** (needed because the LLVM entry edits `LLVM_SEMVER_VERSION` inside `ci.yml`
   itself, and GitHub blocks pushes touching `.github/workflows/*` without this scope even with
   Contents:write) all set to "Read and write", as the `RENOVATE_TOKEN` repository secret so CI
   actually gates these PRs like any other.

**Libraries and code we pull in** (i.e. actual dependencies):

| Source | Freshness (new releases) | Vulnerabilities (CVEs) |
|---|---|---|
| GitHub Actions (`uses: owner/action@sha`) | Dependabot (weekly, Monday) | `dependency-review.yml` (PR-time) |
| Python packages (`requirements.txt`) | Dependabot (weekly, Monday) | `dependency-review.yml` + `osv-scanner.yml` (weekly) |
| C++ `FetchContent` libraries (`cmake/Dependencies.cmake`, `tests/CMakeLists.txt`, `benchmarks/CMakeLists.txt`) | `renovate.yml` (weekly, Wednesday) | `osv-scanner.yml` (weekly) |

Renovate is configured in `.github/renovate.json5`, restricted (via `enabledManagers`) to the
custom regex manager plus Renovate's native `pre-commit` manager (see Tier 1 below), so it never
also tries to manage the ecosystems Dependabot already covers.
It matches this project's `GIT_TAG <sha>  # <tag> - pinned to SHA for supply chain security`
convention and always proposes another full SHA pin, never a floating tag/branch. Each dependency
needs its own `versioningTemplate` because tag conventions differ (`vX.Y.Z` semver for most,
`release-X.Y.Z` for SDL3, `VER-X-Y-Z` for freetype, `vX.Y.Zb-docking` for imgui, which
deliberately restricts matches to this project's tracked docking branch rather than mainline
releases). `nothings/stb` is intentionally excluded: it's pinned to a raw commit with no
upstream tags at all, so there's nothing to compare against -- left as an occasional manual
check. Like Dependabot, Renovate only opens PRs; the same CI gate applies before merge.

**Tools we build with** (cmake, LLVM/clang, Python, ninja, ccache, git -- see
`tools/check-prereqs.sh`): three tiers, all now automated by `renovate.json5` except where noted
below. See #798 for the full repo-wide audit and rationale behind this split.

- *Tier 1 -- auto-PR'd, no gate*: the pre-commit hook tools (`pre-commit-hooks`, `shellcheck-py`,
  `actionlint-py` -- `rev:` pins in `.pre-commit-config.yaml`) via Renovate's native `pre-commit` manager, no
  custom regex needed. The `clang-format` mirror (same file, same manager) is the one exception:
  its formatting behavior tracks the same LLVM major as the compiler toolchain, so its *major*
  bumps are gated exactly like the rest of the LLVM-major process below (a `packageRules` entry
  keyed on the manager's `pre-commit/mirrors-clang-format` dep name) -- minor/patch bumps still
  auto-PR freely. Windows CI has no Renovate-tracked `ninja`/`ccache` pins: `ninja` is
  preinstalled on the `windows-2025` runner image and `.github/actions/setup-windows-llvm/action.yml`
  only verifies its version (bump its `ninja-version` input when the image changes), and
  `ccache` is installed by `hendrikmuhs/ccache-action` from that action's own pinned,
  checksum-verified release binary.
- *Tier 2 -- detected automatically, but only opens a PR after a human ticks the checkbox on
  the Dependency Dashboard issue Renovate maintains* (`dependencyDashboardApproval: true`):
  compiler/interpreter/build-generator bumps that need a deliberate look (new warnings, codegen
  changes, build-semantics changes) before a PR even opens.
  - **LLVM** is pinned across every workflow that runs a compiler or clang-tooling step --
    `.github/workflows/ci.yml`, `heavy-checks.yml`, `release.yml`, and `static-analysis.yml`
    each pin both `LLVM_VERSION` (Linux major) and `LLVM_SEMVER_VERSION` (Windows exact
    version); `codeql.yml`, `sanitizers.yml`, and `copilot-setup-steps.yml` pin the Linux major
    only; `tools/setup-dev.sh` (`LLVM_SUPPORTED_VERSION`, the single literal its own `--llvm`
    guard also reads back from, so the guard can't desync from the value Renovate bumps),
    `tools/setup-dev.ps1` (`$LlvmVersion`), and `cmake/Options.cmake`
    (`TASKSMACK_LLVM_VERSION`, which feeds clang-tidy/IWYU tool discovery) round out the
    per-workflow list. `reusable-build-test.yml`'s own `llvm-version`/`llvm-semver-version`
    input defaults are *also* tracked separately: `ci.yml`'s `build-linux-*`/`build-windows-*`
    jobs call that reusable workflow passing only `os`/`build_type`, never the LLVM inputs, so
    it's the reusable workflow's own defaults -- not `ci.yml`'s env vars -- that actually
    govern the compiler the main build+test jobs use. `.github/actions/setup-windows-llvm/
    action.yml`'s and `.github/actions/setup-llvm/action.yml`'s own `llvm-version` input
    defaults round out the list -- every real caller of both actions passes this input
    explicitly today (all 15 `setup-llvm` call sites and all 5 `setup-windows-llvm` ones), so
    neither is the active bug the reusable-workflow one was, but both are still the actions'
    documented contracts and would silently go stale for a future caller that omits the input.
    `setup-llvm/action.yml` also has two internal bash fallbacks duplicating "22" a second and
    third time for when its input arrives empty. These were briefly removed as apparently dead
    code (every real `uses:` call site in this repo's own workflows passes a value explicitly)
    -- but GitHub Copilot's own code-review setup-step runner invokes this action through a
    path that doesn't resolve `${{ env.LLVM_VERSION }}` and sends an empty string in practice,
    which broke Copilot's own review setup step on this very PR. Restored; kept deliberately
    un-tracked rather than added as a fifth/sixth Renovate manager, since they're a defensive
    fallback for an external caller this repo doesn't control, not a real second pin. All of
    the above are grouped into one PR so they can't drift out of sync. Every
    Linux major-only manager's extraction is anchored to a full stable
    `llvmorg-X.Y.Z` tag (nothing after the patch digit), the same anchoring reasoning as the
    Python interpreter managers below: without it, a prerelease tag like `llvmorg-23.1.0-rc3`
    would reduce to the same "23" a real major release would, putting a premature update on
    the dashboard before a stable LLVM 23 actually exists.

    Every update to this group -- not just major bumps -- requires dashboard approval, unlike
    the rest of this tier. Both CI (`choco install llvm`; Windows jobs restore the installed tree from
    an Actions cache keyed on the exact version and saved only by `main`, so the first `main` runs
    after a bump fall back to Chocolatey) and the dev-box script
    (`winget install LLVM.LLVM`) resolve the pinned Windows version through Chocolatey/WinGet,
    and #752 already recorded a concrete real-world case of that feed lagging upstream
    (upstream had `22.1.8` while Chocolatey only had `22.1.7`) -- an auto-PR'd Windows *patch*
    bump would have generated an unmergeable PR with nothing to catch it before CI ran. A
    human must confirm the proposed version is actually installable through both feeds before
    approving, matching the same manual-verification precedent as the CMake/Ninja/ccache
    dev-box pins below. This supersedes #752's blanket "Windows major bumps are entirely
    disabled" rule with the dashboard-approval mechanism used everywhere else in this tier.

    None of the above tracking makes a major LLVM bump a single-PR, mechanical operation --
    it never has been (see #752), and the dashboard-approval gate exists precisely because a
    human still has to do real work when approving one. Known locations Renovate does *not*
    track, that must be updated by hand as part of that same approval (not exhaustive --
    grep the repo for the old major number too): `CMakePresets.json`'s Linux presets hardcode
    the compiler binary names (`clang++-22`/`clang-22`); `release.yml`'s
    `LLVM_LINUX_EXACT_VERSION` is a separate, manually-verified apt package-version pin (see
    that variable's own comment for the verification command); `tools/pgo.sh`/`tools/pgo.ps1`
    hardcode "LLVM 22" throughout their `llvm-profdata`-version validation and error text; and
    `tools/clang-format.sh`'s version-discovery fallback list (`for ver in 22 21 20 19 18 17`)
    stops at the current major.
  - **Python interpreter**: `.github/actions/setup-python-glad/action.yml`'s
    `python-version: '3.14'`, `.github/workflows/pre-commit.yml`'s `python-version: '3.14'`
    (its own independent pin for `actions/setup-python`, otherwise left stale by a bump to the
    others), `tools/setup-dev.ps1`'s `$PythonVersion` param default (the winget `--id` and the
    `Resolve-Python` install-path probing both derive from that one variable at runtime), and
    `tools/setup-dev.sh`'s `readonly PYTHON_VERSION` (the `python3.14`/`python3.14-venv` apt
    package names and the venv-creation command both derive from it) -- all four grouped
    together so none of them can desync from a version Renovate bumps. This is the interpreter
    *version string* specifically -- not `actions/setup-python`'s own action-version pin, which
    Dependabot already covers separately and always did; the version string passed to it was
    the actual gap. All four managers extract and compare major.minor only, from a *stable* tag
    only (the extraction regex requires the upstream tag to end in a bare patch digit, e.g.
    `v3.14.1`, excluding prerelease tags like `v3.15.0rc2` -- CPython publishes those well
    before the matching stable release, so without this a prerelease could be misread as a
    stable minor bump and proposed before the real release exists). The WinGet package ID this
    repo pins (`Python.Python.3.14`) only exists at major.minor granularity, so a patch-shaped
    proposal like `3.14.1` would rewrite it to a nonexistent ID -- restricting extraction to
    major.minor means a patch-only stable release reduces to the same value already pinned,
    proposing nothing. Both a minor bump (`3.14` -> `3.15`) and a major bump require dashboard
    approval before a PR opens.

    As with the LLVM major checklist above, the four tracked pins aren't the only "3.14" in
    the repo -- this document's own copy-pasteable instructions hardcode it too (the manual
    setup command near the top of this guide, the automated-setup description text, and the
    Windows `winget install Python.Python.3.14` block below), and Dependabot's own comment in
    `requirements.txt`'s header says "Python 3.14". None of these are simple regex-trackable
    pins (they're prose and copy-paste command text, not a single assignment), so they're
    listed here as the known manual-companion-edit checklist for whoever approves a Python
    interpreter bump via the dashboard, same reasoning as the LLVM one.
  - **CMake/Ninja/ccache dev-box pins** in `tools/setup-dev.ps1` (`$CMakeVersion`,
    `$NinjaVersion`, `$CcacheVersion`) are gated on *every* update, not just major, unlike the
    rest of this tier: these track each project's real upstream `github-tags` releases, but
    `tools/setup-dev.ps1` installs via `winget install --version <pin>`, and WinGet's own feed
    lags upstream unpredictably (its CMake feed currently tops out at `3.31.8` while upstream
    already has `3.31.12` -- a same-major patch bump could still propose an uninstallable
    version). No CI job exercises this dev-box installer script, so nothing would catch that
    automatically; a human must manually confirm WinGet catalog availability before approving
    any bump here. See the prerequisite fix below for why these pins exist at all now.
- *Tier 3 -- stays manual, no independent version feed exists to track*: `wpr`/`xperf`/`wpa`/
  `wpaexporter`, CPack's NSIS/dpkg/rpmbuild, the `gh` CLI, `xvfb` -- these ride the OS/runner/SDK
  rather than their own release cadence. Optional local-only tools (Inkscape, heaptrack,
  FlameGraph scripts, hotspot) are also unpinned by design: advisory tooling that never gates
  CI, low priority to track.

**Prerequisite fix that made Tier 2's CMake/Ninja/ccache tracking possible**: `tools/setup-dev.ps1`
previously installed CMake and Ninja via a floating `winget install` with no `--version`, and
ccache with no dev-box pin written down anywhere -- a value can't be tracked for drift if it
isn't recorded somewhere. All three now pin an explicit version: CMake and Ninja match what the
`windows-2025` GitHub Actions runner image itself ships (confirmed directly against
`actions/runner-images`' `Windows2025-Readme.md`, for dev/CI parity); ccache has no CI-side
winget equivalent to mirror (CI gets it from `hendrikmuhs/ccache-action`'s own pinned,
checksum-verified release binary), so it pins the latest version winget actually has available.

**`check-prereqs.sh`'s `MIN_*` floors** (`MIN_CMAKE_VERSION`, `MIN_CLANG_VERSION`,
`MIN_CCACHE_VERSION`, `MIN_GIT_VERSION`, `MIN_PYTHON_VERSION`): these remain **not** automated,
by design, and are unrelated to the tiers above. They're minimum-requirement floors checked
against whatever a contributor already has installed locally, not artifacts to fetch a newer
version of. Raising a floor is a project-policy decision (e.g. "we now use a C++23-modules
feature that needs clang 23"), not a freshness check -- there's no code-level trigger that would
tell a bot when to bump one, and doing so on a schedule risks breaking contributors'
environments with no warning. Bump these by hand when there's an actual reason to.

### Cutting a Release

Pushing a strict `vMAJOR.MINOR.PATCH` tag on `main` is what triggers both an official TaskSmack
release (`release.yml`: validates the tag, builds and signs Linux/Windows packages, and publishes
a GitHub Release) and the corresponding `CHANGELOG.md` update (`changelog.yml`, tag push only --
it has no `workflow_dispatch` trigger). `release.yml` can also be re-run via `workflow_dispatch`
against an existing tag ref (e.g. to retry a failed run without creating a new tag) -- it still
validates that the ref really is a tag either way, so a stray dispatch from a branch can't
impersonate one.

```bash
# 1. Bump CMakeLists.txt's project(TaskSmack VERSION ...) to the target version in its own PR,
#    get it reviewed, and merge it to main. release.yml's validate job requires this to already
#    match the tag (see Release Environment below), so it must land *before* step 3.

# 2. Make sure you are on main and fully up to date with that merged bump
git checkout main
git pull

# 3. Create and push a strict semver tag matching the version just merged — this is the only trigger
git tag v1.0.0
git push origin v1.0.0
```

That single push runs two independent workflows:

- **`release.yml`** validates the tag (see [Release Environment](#release-environment)), builds
  and packages both platforms, validates the packaged artifacts (see [Release Artifact
  Validation](#release-artifact-validation)), signs everything with Sigstore, and publishes the
  GitHub Release (see [Release Artifacts](#release-artifacts) for what it contains and [Release
  Reproducibility](#release-reproducibility) for the determinism policy). Watch it under
  **Actions → Release**.
- **`changelog.yml`** regenerates `CHANGELOG.md` and opens a `chore/changelog-vX.Y.Z → main` pull
  request that must be merged separately -- see [Changelog](#changelog) below. Watch it under
  **Actions → Changelog**.

### Release Artifacts

Each GitHub release (triggered by a `v*.*.*` tag) includes:

- Linux packages: `.tar.gz` and `.deb`
- Windows packages: `.zip`
- Source SBOM: `tasksmack-<label>-source-sbom.spdx.json` (where `<label>` is the release tag with any non-`[a-zA-Z0-9._-]` characters replaced by `-`) — an SPDX-JSON Software Bill of Materials generated from the **source tree** (not from the built Linux/Windows packages) using [Syft](https://github.com/anchore/syft) via [`anchore/sbom-action`](https://github.com/anchore/sbom-action). It lists components and licenses detected in the repository at the tagged commit to improve supply-chain transparency; it does not inventory the contents of the `.deb`/`.tar.gz`/`.zip` packages themselves.

### Release Artifact Validation

`validate-linux-package` and `validate-windows-package` run after each platform's build job and
before `create-release`, so a failure here blocks signing and publishing. Linux installs the
`.deb` in a bare `ubuntu:24.04` container (no dev tools pre-installed) to exercise the runtime
`Depends` declared in `cmake/Packaging.cmake`, separately extracts the `.tar.gz`, and launches
each installed/extracted binary with `SDL_VIDEODRIVER=offscreen` (a real Mesa/llvmpipe GL context,
no X server needed) under a 5-second `timeout`. Windows extracts the `.zip` (requiring
`TaskSmack.exe` directly under the single top-level directory, matching the documented layout) and
launches the executable directly on the runner's desktop session. Beyond the process staying up
for the full 5 seconds, the Linux checks assert on the actual startup log for confirmation that
assets resolved -- an "Assets directory found:" line, no "Icon font not found" warning, and at
least one "Loaded theme:" line -- since `src/UI/AssetPath.cpp` and `Theme.cpp` both fall back
gracefully on missing assets, so a plain liveness check alone would not catch a broken installed
asset layout. These are still smoke checks, not a functional test suite.

### Release Environment

`release.yml`'s `validate` job rejects a tag before any build/sign/publish work runs if it is not
strict `vMAJOR.MINOR.PATCH`, does not resolve to the current `main` HEAD, or does not match
`CMakeLists.txt`'s `project(TaskSmack VERSION ...)` — a missed version bump fails the release
instead of publishing mismatched metadata. Concretely: **bump and merge `CMakeLists.txt`'s
`project(TaskSmack VERSION ...)` to the target version first**, then tag that merged commit on
`main` — see step 1 in [Cutting a Release](#cutting-a-release), which creates the same tag that
also triggers this release workflow. Tagging before the version bump lands fails this check
immediately.

Each platform build job's "Record toolchain versions" step logs the runner image
(`ubuntu-24.04`/`windows-2025`), full compiler/linker versions (`clang --version`, `ld.lld
--version`), CMake, Ninja, and Python versions to that job's log. Linux additionally pins and
asserts an exact `apt.llvm.org` package version for `clang-22` (`LLVM_LINUX_EXACT_VERSION` in
`release.yml`) rather than accepting whatever the apt repository currently serves for the major
version — see the comment above that variable for the deliberate-upgrade procedure. Windows
already pins an exact LLVM semver via `LLVM_SEMVER_VERSION`.

### Release Reproducibility

Release builds set `SOURCE_DATE_EPOCH` (from the tagged commit's timestamp) before configuring.
CMake's `string(TIMESTAMP ...)` calls that populate `version.h`'s `BUILD_DATE`/`BUILD_TIME` honor
that variable, so the embedded timestamp reflects the commit being released rather than
wall-clock build time; CPack's `TGZ`/`ZIP` archive generators also honor it for entry timestamps,
so two independent builds of the same tag produce byte-identical archives. Local/dev builds leave
`SOURCE_DATE_EPOCH` unset and get the real configure-time timestamp, which is fine since dev
builds have no reproducibility requirement.

`release.yml`'s `verify-reproducibility` (Linux) and `verify-reproducibility-windows` jobs each
rebuild their platform's tag from a clean workspace with the same `SOURCE_DATE_EPOCH` and compare
package digests against the corresponding `build-linux`/`build-windows` artifact -- a missing
matching artifact counts as a failure, not a skip. Both are advisory only (`continue-on-error:
true`, not in `create-release`'s `needs`) — a mismatch is surfaced as a workflow warning for
investigation rather than blocking the release, since reproducibility here covers CMake's own
generated files and CPack's archive metadata, not every possible source of binary non-determinism
(e.g. absolute build-path fragments a future dependency might embed in debug info).

To verify a published Linux release yourself: clone the tag, export
`SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)`, build with the `release` preset, package with
`cpack -G "TGZ;DEB"`, and compare `sha256sum` of the result against the published asset.

Every asset is signed with Sigstore (keyless OIDC via `cosign sign-blob`) and shipped with a matching `<file>.bundle`; see the User Guide's [Verifying a Release](docs/guide/user-guide.md#verifying-a-release) for the user-facing `cosign verify-blob` procedure.

### Changelog

[CHANGELOG.md](CHANGELOG.md) is auto-generated by [git-cliff](https://git-cliff.org/) on every `v*.*.*` tag push via the `changelog` workflow. It follows the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format and groups entries by conventional commit type (`feat` → Added, `fix` → Fixed, etc.).

**Do not edit CHANGELOG.md manually.** The `changelog` workflow regenerates the file and opens a pull request that must be merged; it does not push directly to `main`.

#### Producing a Changelog

See [Cutting a Release](#cutting-a-release) for the tag-push steps that trigger this -- the same
tag push also triggers `release.yml`. Once pushed, the `changelog` workflow runs automatically
and:

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
