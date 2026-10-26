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
- [docs/dev/](docs/dev/): the docs site's developer overview and the Wayland test matrix
- [AGENTS.md](AGENTS.md) and [.github/copilot-instructions.md](.github/copilot-instructions.md): agent guidance (also useful to contributors)

Avoid copying contributor commands or architecture diagrams into additional Markdown files. Link to the relevant canonical section instead.

### C++ API Documentation

JSDoc and language-level docstrings are not C++ conventions. When declaration-level API documentation is useful, use Doxygen-compatible `///` or `/** ... */` comments in headers. Document contracts, ownership, units, thread safety, and platform limitations; do not narrate self-explanatory accessors or repeat implementation details. Repository-level design and workflow guidance belongs in the Markdown sources above.

## Quick Start

On a new machine, in this order:

1. **Clone.** On Windows, install git and PowerShell 7 first (see
   [Automated Setup (Windows)](#automated-setup-windows)).

   ```bash
   git clone https://github.com/mgradwohl/tasksmack.git
   cd tasksmack
   ```

2. **Run your OS's automated setup**, below: [Linux](#automated-setup-linux) or
   [Windows](#automated-setup-windows). The script installs the toolchain and Python 3.14 and
   creates the project's Python environment (`.venv`; on Linux with pre-commit in it unless you
   pass `--minimal`, on Windows only with `-DevTools`); then you activate `.venv` and run
   `check-prereqs`. To install the prerequisites by hand instead, see
   [Check Prerequisites](#check-prerequisites).
3. **Configure, build and test** with the [One-Command Dev Workflow](#one-command-dev-workflow):
   `cmake --workflow --preset dev` on Linux, `cmake --workflow --preset win-dev` on Windows.
4. **Before your first pull request**, install the pre-commit hooks, with `.venv` active:
   `pre-commit install` (on Windows, run `pwsh tools/setup-dev.ps1 -DevTools` first). See
   [Pre-commit Hooks](#pre-commit-hooks-recommended).

### Automated Setup (Linux)

Instead of installing prerequisites manually, run:

```bash
./tools/setup-dev.sh          # Install all prerequisites automatically
source .venv/bin/activate     # Use the project-local Python environment
./tools/check-prereqs.sh      # Verify environment after setup
```

The setup script supports Ubuntu and creates `.venv/` for Python 3.14 tooling without changing the system `python` commands. It installs GLAD's hash-locked build dependencies and, unless `--minimal` is used, the development dependencies from `requirements.txt`, plus the coverage/profiling toolchain: `llvm-cov`/`llvm-profdata`, `heaptrack` (see "Linux — Heap allocation profiling" below), and the FlameGraph scripts (see "Linux — CPU profiling" below), cloned to `~/opt/FlameGraph`. Use `--dry-run` to preview what will be installed without making changes, or `--minimal` to install only build tools (skipping coverage/profiling). `perf` itself is deliberately left as a manual step — it's coupled to the running kernel version, and `check-prereqs.sh` detects and prints the exact install command for it.

### Automated Setup (Windows)

A clean Windows 10 or 11 has neither git nor PowerShell 7 (`pwsh`), which the setup script runs
in. Install both with winget (part of Windows' App Installer) from any terminal:

```powershell
winget install --id Git.Git --exact --source winget
winget install --id Microsoft.PowerShell --exact --source winget
```

Then open a **new PowerShell 7 terminal** (`pwsh`; in Windows Terminal, the "PowerShell" profile,
not "Windows PowerShell"), so both are on `PATH`, clone (see Quick Start), and from the
repository root run:

```powershell
pwsh tools/setup-dev.ps1      # Install all prerequisites via winget
```

A normal (not elevated) terminal is fine: the installers ask for administrator approval (UAC) as
they need it. The Visual Studio Build Tools install comes first and is the slow part, often 20
minutes or more, with no output while it runs. When the script finishes, open another new
PowerShell 7 terminal, so the `PATH` and `LLVM_ROOT` it set apply, and from the repository root
run:

```powershell
.venv\Scripts\Activate.ps1    # Use the project-local Python environment
pwsh tools/check-prereqs.ps1  # Verify environment
```

Run these in PowerShell 7. Windows PowerShell (`powershell.exe`) refuses to run
`.venv\Scripts\Activate.ps1` under its default execution policy (`Restricted`).

The Windows setup installs only what building, testing and running need:
- Visual Studio 2022 Build Tools with just the MSVC C++ libraries, and the Windows 11 SDK (10.0.26100) with just its desktop C++ headers and libraries (see [Windows Pre-Requisites](#windows-pre-requisites));
- LLVM 22 (`-LlvmVersion` must stay on the pinned major);
- CMake, Ninja and Python 3.14.

It then creates `.venv\` with Python 3.14 and installs GLAD's hash-locked build dependencies into it. `-DevTools` adds the optional developer tools: ccache and the development dependencies from `requirements.txt` (pre-commit and the other Python tools). You need those before your first pull request, not to build. clang-tidy and clang-format come with LLVM. CMake configures GLAD with `.venv\Scripts\python.exe` whenever it exists (`.venv/bin/python` on Linux). Re-running the script is safe: winget's "already installed" results count as success, and it skips the Windows SDK when its 10.0.26100 desktop headers and libraries are already there. `check-prereqs.ps1` reports the LLVM in `LLVM_ROOT` (what the `win-*` presets build with), marks tools on an LLVM major other than CI's pinned one with `!`, and marks the optional developer tools with `!` when they are missing rather than failing: ccache, clangd and clang-format (found in `LLVM_ROOT\bin`; the LLVM installer does not add itself to `PATH`, which VS Code needs for them), and the Windows Performance Toolkit (`wpr`, `xperf`, `wpa`) for ETW profiling.

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
./tools/check-prereqs.sh        # Linux
pwsh tools/check-prereqs.ps1    # Windows
```

To check the whole newcomer path on Windows, from a clean machine to a built, tested and launched
TaskSmack, run `tools/newcomer-check.ps1` on a throwaway Windows, such as Windows Sandbox (#2618).
It installs software machine-wide, so don't run it on your own machine. It follows this file's
steps in order, checks that the docs still give each command it runs, and stops at the first
place a newcomer would trip (`-ContinueOnFinding` records them all). It writes a step table to
`results.md` in `-OutDir`. Its help (`Get-Help tools/newcomer-check.ps1 -Full`) has the Windows
Sandbox options.

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
- FreeType: nothing to install; the build always fetches and builds the version pinned in `cmake/Dependencies.cmake` and links it statically
- **Optional GPU monitoring libraries:**
  - NVIDIA drivers with NVML (libnvidia-ml.so) for NVIDIA GPU support
  - ROCm SMI library (librocm_smi64.so) for AMD GPU support
  - Intel: no package needed — reads `/sys/class/drm` via sysfs (kernel DRM support is standard on all modern Linux kernels)

Example (Ubuntu/Debian):

```bash
# clang-22 comes from apt.llvm.org, and Ubuntu 24.04's own cmake (3.28) is older than 3.29, so add
# apt.llvm.org's and Kitware's repositories first (tools/setup-dev.sh does both)
sudo apt install clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22 libc++-22-dev libc++abi-22-dev cmake ninja-build ccache

# Python 3.14 (Ubuntu 24.04's python3 is 3.12; tools/setup-dev.sh adds the deadsnakes PPA for it),
# with GLAD's hash-locked dependencies in the project venv
sudo apt install python3.14 python3.14-venv
python3.14 -m venv .venv && source .venv/bin/activate
python -m pip install --require-hashes -r requirements-glad.txt

# Optional: For GPU monitoring
# NVIDIA drivers (download from nvidia.com)
# ROCm SMI (download from amd.com/rocm)
# Intel: no package needed (reads /sys/class/drm via sysfs)

# Optional, but needed for everything TaskSmack reads or controls over D-Bus or the journal: the
# Services tab (systemd), and on the System Information page Boot performance (#1525), disk SMART
# health (udisks2), crash details (systemd-coredump's journal entries), and each network adapter's
# DNS, Wi-Fi and DHCP details (systemd-resolved, NetworkManager, systemd-networkd). Found with
# pkg-config at configure time; without it those say they are unavailable. tasksmack-helper links it
# too, to read every user's crash entries from the journal.
# -DTASKSMACK_WITH_SDBUS=OFF builds without it even when it is installed.
sudo apt install libsystemd-dev pkg-config
```

### Windows Pre-Requisites

- LLVM/Clang 22 (includes clang-tidy, clang-format, lld, llvm-cov, llvm-profdata)
- `LLVM_ROOT` environment variable set
- CMake 3.29+
- Ninja
- ccache 4.9.1+ (optional, for faster rebuilds; `setup-dev.ps1 -DevTools`)
- Python 3.14+ with jinja2 (required for GLAD OpenGL loader generation)
- git (CMake requires it, and fetches the dependencies with it)
- FreeType: nothing to install; the build always fetches and builds the version pinned in `cmake/Dependencies.cmake` and links it statically
- **MSVC C++ libraries and the Windows SDK**: required. Clang targets the MSVC ABI, so it compiles and links against the MSVC standard library and CRT and the Windows SDK. It finds the MSVC libraries through the Visual Studio installation and the SDK through the Windows Kits registry key. Nothing else from Visual Studio is used: not MSBuild, its CMake or Ninja, `rc.exe` or `cl.exe`, and no SDK tool. `tools/setup-dev.ps1` installs Visual Studio 2022 Build Tools with only `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` (plus `...VC.Tools.ARM64` on Windows on Arm). It installs the Windows SDK 10.0.26100 separately (winget `Microsoft.WindowsSDK.10.0.26100`) with only its desktop C++ feature for your architecture (`OptionId.DesktopCPPx64`, or `OptionId.DesktopCPParm64` on Arm), and skips it when `Windows Kits\10\Include\10.0.26100.0` and the matching `Lib` folders are already there. A Visual Studio you already have works too if it has `VC.Tools.x86.x64` and a Windows 11 SDK 10.0.26100 (its `Windows11SDK.26100` component, or the standalone SDK). See below for how the RC (resource) compiler is selected.

**RC (resource) compiler**: the build compiles `assets/tasksmack.rc.in` (icon and version info) into
`TaskSmack.exe`; the DPI-awareness manifest is embedded separately at link time. `cmake/RCCompiler.cmake`
requires `llvm-rc`, which comes with the LLVM install above, and fails configuration with an actionable
error if it can't find it. There's no fallback to the Windows SDK's `rc.exe`, which hangs on this project
(#746), and no need for a Visual Studio Developer Command Prompt. To use another RC compiler, set the `RC`
environment variable before configuring (it may include arguments, and a path with spaces must be quoted
inside the value: `RC="\"C:\Program Files\LLVM\bin\llvm-rc.exe\""`), or pass
`-DCMAKE_RC_COMPILER=<path>`. Use `RC` to pin `rc.exe` itself: a `-DCMAKE_RC_COMPILER` that points at
`rc.exe` with no earlier selection recorded is treated as a stale cache entry and replaced by `llvm-rc`.

Install Python + jinja2:

```powershell
winget install Python.Python.3.14
py -3.14 -m venv .venv
.venv\Scripts\python.exe -m pip install --require-hashes -r requirements-glad.txt
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
- **fix-byte-order-marker**: Remove UTF-8 byte-order marks
- **check-utf8**: Fail on text files that aren't valid UTF-8, or that hold double-encoded text, such as an
  em dash read as Windows-1252 and saved again as U+00E2 U+20AC U+201D (#2529) (`tools/check-utf8.py`)
  -- see [Text encoding and locale](#text-encoding-and-locale)
- **check-guidance-sync**: Fail when the AI-agent guidance drifts (`tools/check-guidance-sync.py`): the
  `.claude/skills/tasksmack-review/SKILL.md` body must match `.github/skills/code-review/SKILL.md`
  (frontmatter ignored), and `CLAUDE.md` must import [AGENTS.md](AGENTS.md) with an `@AGENTS.md` line
- **check-cache-prune-triggers**: Fail when a workflow that saves Actions caches (ccache, `actions/cache`, a
  repo cache action's `mode: save`, `setup-windows-llvm`, or a reusable workflow that does) is missing from
  `cache-prune.yml`'s `workflow_run` list (`tools/check-cache-prune-triggers.py`, #2410)
- **check-cflite-paths**: Fail when `cflite_build.yml`'s `push` paths and `cflite_pr.yml`'s `pull_request`
  paths differ, so every path PR fuzzing watches also refreshes the main baseline it fuzzes against
  (`tools/check-cflite-paths.py`, #2533)
- **check-dll-loads**: Fail when `src/` loads a DLL with bare `LoadLibrary`/`LoadLibraryA`/`LoadLibraryW`,
  or `LoadLibraryEx*` without a `LOAD_LIBRARY_SEARCH_*` flag; either searches the application directory
  first, so a planted DLL next to a portable `TaskSmack.exe` would load (`tools/check-dll-loads.py`, #2530)
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

TaskSmack builds only 64-bit targets: x64 and ARM64 (Linux aarch64 and Windows ARM64, #2214). Configuring for a 32-bit
target fails at configure time.

This repo uses CMake Presets; list them with:

```bash
cmake --list-presets
```

Ninja runs one compile job per core (plus two) by default. On a machine short of memory, such as a WSL 2 VM (which
gets half the host's RAM by default), cap the jobs: `cmake --build --preset debug -j 4`, or
`export CMAKE_BUILD_PARALLEL_LEVEL=4` before `cmake --workflow`, which takes no `-j`.

### Source encoding

Every TaskSmack target compiles its sources as UTF-8, with UTF-8 narrow string literals
(`/utf-8` with clang-cl, `-finput-charset=UTF-8` with clang/gcc), and every Windows target defines
`UNICODE` and `_UNICODE`, so an unsuffixed Win32 macro resolves to its `W` function. Call the
`W` function by name, converting with `Platform::WinString::utf8ToWide` at the call site; don't use `TCHAR` or the
`A` functions. `tasksmack_apply_text_encoding()` in `cmake/CompilerOptions.cmake` sets both; call
it on any new target. Third-party dependencies keep their own settings.

### Text encoding and locale

- **UTF-8 `std::string` everywhere.** Strings inside TaskSmack hold UTF-8. Lengths and offsets
  are byte counts, not characters or columns.
- **UTF-16 only at the Win32 call site.** Convert with `Platform::WinString::utf8ToWide` /
  `wideToUtf8` right where you call the `W` API, and convert straight back. Don't use `TCHAR`,
  `std::wstring` members or the `A` functions.
- **The process runs in UTF-8.** On Windows the application manifest sets the active code page
  to UTF-8 (`activeCodePage`, Windows 10 1903+), so `GetACP()` is 65001 and narrow CRT calls,
  `std::filesystem::path` and `getenv` take UTF-8. The test binaries have no manifest, so code
  under test must not depend on it: convert explicitly rather than relying on the code page.
- **Paths meet UTF-8 text only through `Core/Utf8Path.h`.** `Core::pathToUtf8(path)` for a log
  line or a library that takes a UTF-8 file name (toml++'s `parse_file`, SDL), and
  `Core::utf8ToPath(text)` for a path read from UTF-8. Never `path.string()` or
  `std::filesystem::path(std::string)`: on Windows both go through the code page, so in a binary
  without the manifest (the tests, `TaskSmackUiTraining`) they throw for CJK or emoji and turn
  accented Latin into bytes that aren't UTF-8. To open a file, pass the path itself (`std::ifstream`,
  `std::filesystem`) or `path.native()`/`wstring()` to a `W` API, never a narrowed name to `fopen`
  or an `A` API. `tests/App/test_NonAsciiRoundTrips.cpp` covers the config, themes, log, selection,
  process names and opener with CJK, emoji, accented Latin and Arabic names.
- **The log file keeps `SPDLOG_WCHAR_FILENAMES`** (#1648): spdlog then opens it by its UTF-16 path
  (`Core::makeLogFileSink`, which static_asserts the macro on Windows). Without it spdlog takes a
  narrow name and `_fsopen`s it through the code page, which writes the log into a mojibake
  directory in any binary without the manifest. Define it on every Windows target that logs to a
  file.
- **Locales are set once, at startup** (`Core/LocaleSetup.h`, first thing in `main`): the C and
  C++ locales follow the user's regional settings with a UTF-8 codeset, and the C runtime's
  `LC_NUMERIC` stays `"C"`. Don't call `setlocale` or `std::locale::global` anywhere else. The
  startup log records the result (`Locale: GetACP()=... C="..." C++ global="..." user="..."`).
- **Displayed numbers use the user's locale, never the global one** (#1648, slice E): format them
  with `UI::Format` (`formatIntLocalized`, `formatFixedLocalized`/`formatFixedLocalizedTo`,
  `formatBytes`, `formatPercent`, the axis formatters). It reads the display punctuation (decimal
  mark, grouping, and the thousands separator as whole UTF-8, e.g. fr-FR's U+202F) that `main`
  takes once from `Core::LocaleSetup::userNumberPunctuation()`: a plain struct, no `std::locale`
  per call. Don't use `std::format("{:L}")`, `{:.1f}`, `%f` or a hard-coded `.`/`,` for a number
  the user sees; the global C++ locale has the classic numeric facets, so `"{:L}"` prints classic
  text, and `numpunct<char>` can't hold a multibyte separator anyway. Units and SI/IEC prefixes
  don't change. Tests pin the punctuation with `UI::Format::ScopedDisplayPunctuation` (classic
  by default); `tests/TestLocales.h` gives the real en-US/de-DE/fr-FR punctuation, skipping a
  locale the machine lacks.
- **Copy follows the display:** text the user copies from the UI (System Information's Copy) is
  the shown text, in the user's locale.
- **Parsing and machine-readable output use the classic locale:** `std::from_chars`/`std::to_chars`,
  `std::format` without `L`, or a stream imbued with `std::locale::classic()`. This covers config
  and theme TOML, `/proc` and `/sys`, command-line values, structured log fields and the Render
  Metrics overlay's "Copy CSV" (profiling data for scripts, not a user export). Never use
  `std::stod`/`strtod`/`atof`/`sscanf` or a default-locale stream on text you read back.

**Files and tooling:**

- **Every text file in the repository is UTF-8 without a BOM**, with LF line endings
  (`.editorconfig`, `.gitattributes`). No UTF-16 files, `.rc` resources included:
  `assets/tasksmack.rc.in` is ASCII. llvm-rc rejects non-ASCII string literals unless it's given
  `/C 65001`, so add that flag if a resource ever needs non-ASCII text.
- **pre-commit enforces it.** `fix-byte-order-marker` strips UTF-8 BOMs, and the local `check-utf8`
  hook (`tools/check-utf8.py`) fails on any staged text file that doesn't decode as strict UTF-8,
  reporting `file:line:column`. Binary files (the types `.gitattributes` marks binary) and the fuzz
  corpora under `tests/fuzz/corpus/` are skipped.
- **PowerShell scripts need PowerShell 7 (`pwsh`)** and name the encoding on every write:
  `Set-Content`/`Add-Content`/`Out-File -Encoding utf8`, which in PowerShell 7 means UTF-8
  without a BOM (`utf8NoBOM` is the same thing). Use `-Encoding ascii` only for `.cmd` stubs that
  `cmd.exe` reads. A script that parses a native tool's output (git, cmake, clang-tidy, llvm-cov)
  sets the console encoding to UTF-8 first, so PowerShell decodes that output as UTF-8 rather
  than the OEM code page:

  ```powershell
  $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
  try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }
  ```

  Pass `-c core.quotePath=false` to git commands that list paths, so non-ASCII names come out as
  UTF-8 rather than quoted octal escapes.
- **Python tools** pass `encoding="utf-8"` to every `open()`/`read_text()`/`write_text()` and to
  every `subprocess` call that reads text (add `errors="replace"` for output from tools outside the
  repository). Wrappers that start them run Python in UTF-8 mode: `python -I -X utf8 ...` (`-I`
  ignores `PYTHONUTF8`), or `export PYTHONUTF8="${PYTHONUTF8:-1}"` where Python isn't isolated.

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
- `release-compatible` (Linux) or `win-release-compatible` (Windows) for x86-64-v2, which needs SSSE3, SSE4.1, SSE4.2, POPCNT and CMPXCHG16B (Intel Nehalem or later, AMD Bulldozer or later, or AMD Jaguar). AMD Athlon II / Phenom II and Intel Core 2 do not qualify.
- `release` (Linux) or `win-release` (Windows) for default compiler optimizations, targeting the baseline x86-64 that every x86-64 CPU runs

You can also customize the target microarchitecture by setting the `TASKSMACK_MARCH` CMake variable:

```bash
cmake --preset release -DTASKSMACK_MARCH=native  # Optimize for your specific CPU
cmake --preset release -DTASKSMACK_MARCH=x86-64-v2  # Target SSE4.2/POPCNT CPUs
```

### Common Presets

| Preset (Linux) | Preset (Windows) | Description |
|----------------|------------------|-------------|
| `debug` | `win-debug` | Debug symbols, no optimization, security hardening |
| `relwithdebinfo` | `win-relwithdebinfo` | Debug symbols + optimization |
| `release` | `win-release` | Optimized, no debug symbols |
| `release-compatible` | `win-release-compatible` | Release build for older CPUs (x86-64-v2: SSE4.2/POPCNT CPUs) |
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
- Linux: `tasksmack-helper` is built beside it, but TaskSmack only uses the installed
  `/usr/libexec/tasksmack/tasksmack-helper` with its polkit policy. To try **Authenticate and retry**
  from a source build, install a `.deb`/`.rpm` built with `cpack` (see Packaging below), or install
  just that component from a `release` build (which links libc++ statically) with
  `sudo cmake --install build/release --prefix /usr --component helper`.

## Test

```bash
# Linux
ctest --preset debug

# Windows
ctest --preset win-debug
```

ctest runs each Google Test **suite** as one entry, in one process (#1871), not each case: on Windows,
starting a process for each of the ~5,000 cases took most of the test time. `cmake/GTestSuites.cmake`
registers the entries from the binaries' suite lists when they link. `ctest -R` matches suite names, and
`-L gtest` / `-LE gtest` picks or skips all of them (the script tests have no `gtest` label), and
`-L pwsh-script` the Windows tests that run under PowerShell 7:

```bash
ctest --preset debug -R ProcessModelTest       # one suite (ApplicationTest runs as ApplicationTest/shard1of3 ...)
ctest --preset debug -LE gtest                 # only the script tests (BenchScript, TidyCacheScript, ...)
ctest --preset win-debug -L pwsh-script        # only the pwsh script tests (EtwCaptureScript, SetupDevScript, ...)
```

To run one case, or a pattern of them, run the binary with `--gtest_filter`:

```bash
build/debug/tests/TaskSmackTests --gtest_filter='ProcessModelTest.WhenConstructedWithNullProbe'
build\win-debug\tests\TaskSmackTests.exe --gtest_filter=ProcessModelTest.*Gpu*
```

- **Output and results.** `--output-on-failure` prints the whole suite's gtest output. Each entry also
  writes gtest's own XML report, one testcase per case, to `build/<preset>/gtest-results/`. CI folds
  those into one JUnit file (`tools/merge-gtest-results.py`) for the Job Summary and the test-results
  artifact, so each case is still listed.
- **Isolation.** Cases in one suite share a process. A case must not depend on running alone, or leave
  state behind that breaks the next case (a static, an environment variable, a library left loaded).
- **Timeouts.** An entry's 300 s `TIMEOUT` covers its whole suite. Reading a binary's suite list when it
  links (`--gtest_list_tests`) has 120 s: a loaded CI runner linking three executables at once passed
  the 30 s default (#2838).
- **Options.** `tasksmack_gtest_add_suites()` takes `SHARDS <Suite>=<n>` to split a slow suite into `n`
  entries (via gtest's own sharding), `ISOLATE <Suite>` to run each case of a suite in its own process,
  as `gtest_discover_tests()` did, and `SUITE_PROPERTIES <Suite> <name> <value>` for one suite's entries
  (the suites that read back the Windows system clipboard share a `RESOURCE_LOCK`).

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

### Property-based tests

For code whose contract holds over a whole input space -- formatters, parsers, round-trips -- write a
[rapidcheck](https://github.com/emil-e/rapidcheck) property next to the example-based tests. `RC_GTEST_PROP`
registers it as an ordinary Google Test case, so it runs under `ctest` with everything else; each run checks
a few hundred generated inputs and shrinks a failure to the smallest one it can find, printing it with a
seed to replay. Examples: `tests/UI/test_FormatProperties.cpp` (digit grouping, integer formatting) and
`tests/Platform/test_NetworkTextProperties.cpp` (address text, the networkd JSON parser).

```cpp
RC_GTEST_PROP(MyTest, RoundTrips, (std::uint32_t value))
{
    RC_ASSERT(parse(format(value)) == value);
}
```

Keep a property's generator bounded (no unbounded loops on what it generates), and use `RC_PRE` to drop
inputs a property doesn't cover rather than asserting around them.

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
`createDump()` (#807) returns what `setDumpResult(...)` set, or with `setDumpWaitsForCancel(true)`
blocks until its stop token is requested, for tests of a dump's Cancel; `createDumpCount()` and
`lastDumpDirectory()` record the call. `dumpSpace()` (#2001) returns what `setDumpSpace(...)` set
(unknown by default), counted by `dumpSpaceCount()` with the folder in `lastSpaceDirectory()`;
`chooseDumpFolder()` returns what `setFolderChoice(...)` set (cancelled by default), counted by
`chooseFolderCount()` with `lastFolderInitial()` and `lastFolderOwner()`. A test that lets a folder
be chosen gives its `ProcessDumpView` a handler of its own, so nothing is saved to the user's config.

For `Platform::IProcessEnvironmentReader` (the on-demand environment read behind Process Details'
Environment section, #179), use `TestMocks::MockProcessEnvironmentReader`: `setResult(...)` and
`setHasEnvironment(...)` configure it, and `readCount()`/`lastTarget()` let a test check the read
cadence and the target. `Platform::IProcessConnectionsReader` (the Connections section, #799) has
`TestMocks::MockProcessConnectionsReader` on the same terms (`setResult(...)`,
`setHasConnections(...)`, `readCount()`, `lastTarget()`).

### Adding a System Information section

The System tab (#1399) is a list of sections, each `{title, icon, rows}` with rows
`{label, value, unavailableReason, isIdentifier}` (`src/App/Panels/SystemInfoSections.h`). The
Operating system section (#1512) is the pattern to follow:

1. **Platform:** a raw struct of the section's facts in `src/Platform/ISystemInfoProbe.h` and a
   `read<Section>()` method on `ISystemInfoProbe` (and its unsupported stub), implemented in
   `WindowsSystemInfoProbe` and `LinuxSystemInfoProbe`. Leave a fact it can't read empty or 0; no
   labels or formatting here. Put the parsing in pure helpers with fixture tests: a
   `*Math.h` under `Platform/Windows` (tests in `tests/Platform/WindowsMath`), or a standard-library-only
   Linux header that reads under an injected root, like `Platform/Linux/LinuxOsInfo.h`.
2. **Domain:** a member of `SystemInfoSnapshot`, filled in `SystemInfoModel::read()`. Reads happen
   once on a worker thread when the tab first shows, and on Refresh: never per tick or per frame.
3. **App:** a `build<Section>Section()` in `SystemInfoSections.cpp` turning the facts into rows, and
   one line in `buildSystemInfoSections()`. Give an empty value an `unavailableReason` (it shows as a
   muted "—" with that tooltip and is copied as "unavailable (reason)"), and mark anything that names
   the user, the machine or the organisation (serial numbers, MAC addresses, UUIDs, product IDs)
   `isIdentifier`, so it stays hidden and out of Copy until "Show identifiers" is ticked. The filter,
   Copy and layout then work with no further code.
4. **Tests:** the parsers; the section's rows (`tests/App/test_SystemInfoView.cpp`), including
   which rows are identifiers; a smoke test of the real probe on its platform.
5. **Docs:** the section's bullet under "System Information" in `docs/guide/user-guide.md`, and a
   Platform Differences row.

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
   (`dispatchProcessAction`), and `UI/DpiScale.h` for the pattern. When
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

4. **Need the whole app (a real window, GL, the real theme and fonts)?** Put the test in
   `APP_REAL_TEST_SOURCES` in `tests/CMakeLists.txt`. It builds into `TaskSmackAppTests`, which links
   the app's own objects (`TaskSmackApp`, the way the benchmarks do), so a real `Core::Application`
   can run `UILayer` and other layers through `run()`. Push a driver layer last, so its `onRender()`
   runs inside the frame after the others have drawn. Call `Core::AnimationRequest::request()` to
   keep frames coming, and press widgets by ID with `ImGui::ActivateItemByID()`. Skip the test when
   no display with GL 3.3 is available; with `TASKSMACK_REQUIRE_DISPLAY=1` (CI: Linux under Xvfb,
   Windows with Mesa's llvmpipe) a missing display fails instead. Its own `main()` (`tests/App/AppTestsMain.cpp`) points
   `TASKSMACK_CONFIG_DIR` at a fresh temporary directory before any test runs, so whatever the app
   saves (`ShellLayer::onDetach()` writes `config.toml`) never reaches your own config.
   `tools/coverage.sh` runs this binary too. Example: `tests/App/test_UiLayersOnDisplay.cpp`, the title
   bar's buttons through `TitleBarLayer`, and the whole shell as `main.cpp` composes it.

   **The UI scenarios** (`tests/App/test_UiScenariosOnDisplay.cpp`, ctest entry `UiScenarioTest`, #2711)
   drive the whole shell through the steps a UI dependency bump is signed off on:
   - **The steps:** the Processes list, then its tree and a collapsed branch; typing in the filter; the row
     menu; a column reordered and resized by dragging; Process Details' Overview and Network and I/O; the
     machine overview; System Information; Help; About; and a theme picked in Settings and saved.
   - **How each one works:** a step injects its input through ImGui (mouse and key events, or
     `ActivateItemByID()` for a button or tab), then waits, frame by frame, until the shell or ImGui shows
     the effect. It asserts behaviour, never pixels.
   - **The machine:** the ctest entry sets `TASKSMACK_SYNTHETIC` for its process, so the processes and charts
     are the same on every runner. System Information still reads the real machine.
   - **Capturing frames:** with `TASKSMACK_UI_CAPTURE_DIR=<dir>` set, each finished step's frame is saved
     there as `<NN>-<step>.png`, read back before it is presented. The variable is read by this test only;
     it isn't one of the app's hooks.

     ```bash
     TASKSMACK_UI_CAPTURE_DIR=/tmp/ui xvfb-run -a env -u WAYLAND_DISPLAY ctest --preset debug -R UiScenarioTest
     ```

   - **Adding a step:** add it to `signOffSteps()`. Each step starts where the last one left the shell, and the
     first step that isn't done within its frame budget fails the test and names itself.
   - **Comparing two refs:** `tools/ui-compare.sh <ref-a> <ref-b>` builds both refs in scratch worktrees (the
     debug preset, `-j 4`, one at a time), runs the scenarios in capture mode on each, and writes a report
     with an `index.html` into `ui-compare-<a>-vs-<b>/`. It shows each step's two frames, a diff with the
     changed pixels in red and the share of pixels that changed, and each run's warnings, errors and ImGui
     assertions. Use it to sign off a UI dependency bump (ImGui, ImPlot, SDL, FreeType):

     ```bash
     tools/ui-compare.sh origin/main renovate/imgui-1.94
     ```

     - **Dependencies:** each worktree reuses this checkout's FetchContent sources read-only for every
       dependency it pins at the same commit. The bumped one is downloaded.
     - **Reading the numbers:** live values (CPU %, times) and System Information's real-machine facts move a
       little between runs, so a small change on every step is normal. Look at the diffs.
     - **Where it runs:** under Xvfb with `WAYLAND_DISPLAY` unset, as CI runs it, even when a desktop is up,
       because a real pointer's moves mix with the injected ones. `--use-display` runs it on the desktop.
     - **Requirements:** both refs need the scenario suite.

Before writing any of these tests, check whether the logic already exists as a shared, tested
helper (for example `Core::isEnvFlagEnabled()` in `Core/EnvUtils.h`), and reuse it instead.

### Testing Linux D-Bus readers (systemd, NetworkManager, udisks2)

The readers declared in `Platform/Linux/SystemdBus.h` ask system services over D-Bus, one file per
service (`SystemdUnits.cpp`, `UDisks2Smart.cpp`, `NetworkManager.cpp`, `NetworkdResolved.cpp`, sharing
`SdBusCall.h`), and most machines (WSL, the CI runners) don't run NetworkManager, systemd-resolved, systemd-networkd or udisks2. So the live tests in
`tests/Platform/test_SystemdBus.cpp` usually skip. To test a reader's parsing, use
`tests/Platform/FakeSystemBus.h` instead:

- It starts a private `dbus-daemon` in a temporary directory and points `DBUS_SYSTEM_BUS_ADDRESS` at
  it, so the reader's own `sd_bus_open_system()` reaches it. The variable is restored and the daemon
  reaped afterwards.
- Describe the service before `start()`:
  - `properties()`, a map keyed by object path, then interface, then property name, answers
    `org.freedesktop.DBus.Properties.Get`, with values of type `s`, `o` (`ObjectPath`), `u`, `ay`, `a{sv}`, `b`, `t`, `d`, `x`, `q`, `as`, and
    resolved's `a(iay)` and `a(sb)`;
  - `addStringMethod()`, `addIntMethod()` and `addNoArgMethod()` add methods returning a path (`o`), a
    name and path (`so`), a string (`s`), an `a{sv}`, or a named D-Bus error.
- `start(serviceName)` takes the name on the bus. Pass `""` for a bus where the service isn't running,
  to test the reader's "isn't running" path.
- A path with nothing on it answers `UnknownObject`, as a real service does.
- Skip the test when `FakeSystemBus::available()` is false (no `dbus-daemon` on `PATH`). The CI images
  have it, so these tests run there.

Examples: `tests/Platform/test_SystemdBusFake.cpp` (NetworkManager, resolved, networkd, udisks2).

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
   `clampNonNegativeQuadPart`).
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

### Documentation screenshots

`tools/screenshots.sh` (Linux; under Xvfb when `$DISPLAY` is unset) and `tools/screenshots.ps1`
(Windows) regenerate `docs/images/screenshots/<os>/` (#2827): one launch of the built app per shot,
sequentially, with no injected input. Each run gets its own `TASKSMACK_CONFIG_DIR` with a generated
`config.toml` (theme, medium font, 1 s sampling, a 1440x900 window, the Processes table's Command
column off, since command lines name the machine's users and paths), the startup hooks from the next
section that pick the view, and `TASKSMACK_SCREENSHOT`, which writes the frame in-app and quits. The
matrix is Overview in six themes (`windows-dark`, `ubuntu-light`, `dracula`, `nord-light`, `gruvbox`,
`monochrome-light`), Processes and System Information in the first dark and the first light theme,
the machine tab's CPU Cores, GPU and Network and I/O sub-tabs (`cores`, `gpu`, `network`, selected by
`TASKSMACK_MACHINE_TAB`, #2869), Process Details (overview, GPU, network, and the Overview with its
Modules, Open files and Connections sections expanded by a one-step `TASKSMACK_DEMO` script:
`details-modules`, `details-openfiles`, `details-connections`, and on Linux, the only platform that
reads them, Environment and Security too: `details-environment`, `details-security`) for one process,
Services, Startup, Settings (`TASKSMACK_OPEN=settings`), About and Help, named `<view>-<theme>.webp`,
plus three animated WebPs at 12 fps built from frame sequences:
`overview-live.webp` (8 s of the Overview), `walk-live.webp` (10 s: `TASKSMACK_DEMO` opens the
Processes table, selects the process and brings up its Process Details' Network tab) and
`themes-live.webp` (9 s: the Overview through the six themes, 1.5 s each). The `overview-*` shots and
`themes-live` get `[sampling] history_max_seconds = 60` and are captured after 60 s (the hook's
maximum), so their history charts are full; the other views are captured after 8 s (the GPU and
network charts need a few samples). The expanded-section shots get their own window size,
`--section-window` / `-SectionWindow`: 1440x1200 on Linux, where the taller Actions and Priority cards
push every section below a 900 px fold, and `-Window`'s 1440x900 on Windows.
`--webp` / `-WebP` converts with `cwebp -q 80` and `img2webp` from libwebp (`sudo apt install
webp`; Windows: `winget install Google.Libwebp`); without it the PNGs are copied and the frames left
in the work directory instead. Both scripts print a table of exit code, seconds and bytes per shot
and exit non-zero if any shot failed.

Regenerate both sets at each minor release, from the release candidate's build, and commit them with it:

```bash
# Linux (WSL is fine): a throwaway host and user name (unshare -r -u) so no real name reaches the images.
# SDL_VIDEO_X11_SCALING_FACTOR=1.75 renders at the Windows set's 175 % scale (SDL reads that hint,
# Xft.dpi or GDK_SCALE, never Xvfb's -dpi); the expanded-section shots are taken at --section-window,
# 1440x1200 by default, since on Linux the Actions and Priority cards are taller and push the sections
# below a 900 px fold.
cmake --preset release && cmake --build --preset release
SDL_VIDEO_X11_SCALING_FACTOR=1.75 tools/screenshots.sh --webp --hostname tasksmack-demo
```

Under Xvfb, SDL warps the pointer to the window's centre at startup, which hovers a chart and draws its
tooltip into the shot: keep the pointer parked outside the window for the run (python-xlib's
`root.warp_pointer()` to a screen corner, repeated every quarter second). The selected process's command
line is shown in Process Details, so run the binary from a path that names no user (a bind mount of the
build's `bin/` at `/srv/tasksmack` inside the namespace, `--exe /srv/tasksmack/TaskSmack --process TaskSmack`).

```powershell
# Windows: in a Windows Sandbox (its own processes, user and host name) with a win-release build made
# inside it (App Control there refuses a binary built on the host), and Mesa's llvmpipe opengl32.dll
# beside TaskSmack.exe when the Sandbox has no vGPU
.\tools\screenshots.ps1 -Exe build\win-release\bin\TaskSmack.exe -Out C:\shots\out   # then cwebp/img2webp on the host
```

The images show the machine they were taken on. Before committing, look at every one for a user name,
host name or process you would not publish: the Processes User column, System Information and the
hostname tab are the usual places.

The user guide (`docs/guide/user-guide.md`) and `README.md` embed the images by these fixed names
(`docs/images/screenshots/windows/processes-windows-dark.webp`, `.../linux/overview-live.webp`, and so
on), so a regeneration that keeps the matrix replaces every picture in place and needs no doc edit.
Renaming a shot or dropping one from the matrix is a doc change too: `tools/md-link-audit.sh` (the
"Audit markdown links" step in `ci.yml`) fails on a Markdown image whose file is missing, so run it
after the new set is in. The guide uses the Windows image wherever the Linux set has no
counterpart of the same name; once the Linux set has one, switch the guide to it in the same PR.

**At a minor release** (the `vX.Y.0` candidate, see [Cutting a Release](#cutting-a-release)):
regenerate both sets from the candidate's build (the Linux set under Xvfb in WSL, the Windows set in a
Windows Sandbox, as above), review every image for identity, and merge them in one docs PR to `main`
with the `backport:X.Y` label so `release/X.Y` carries the same pictures. Images are documentation, so
the PR merges under release gating without waiting for the tag.

### Visual validation without input (Process Details)

Agents must not inject clicks or keys, so nine test-only environment variables (#1559, #172, #2827,
#2869, #2923), read once at startup, select a process, open a tab or a sub-tab, open the Help window, the About
dialog or Settings, replay a timed script of such requests, show the Render Metrics overlay and write its CSV, or write a screenshot of the window and quit, by themselves. With
`TASKSMACK_SELECT_PID` or `TASKSMACK_SELECT_NAME`, the process is selected when it first appears in a
snapshot, exactly as a click and the row menu's **Details** do, and Process Details opens. If it has not
appeared after 20 snapshots, one warning is logged and nothing is selected. While any of the nine
variables, or `TASKSMACK_CONFIG_DIR` below, is set, the startup "Limited Data" notice is not shown, since
it would cover the capture. Unset, the variables do nothing.

| Variable | Value |
|---|---|
| `TASKSMACK_SELECT_PID` | A PID. Wins over `TASKSMACK_SELECT_NAME`. |
| `TASKSMACK_SELECT_NAME` | An executable name, e.g. `explorer.exe`; the first match. Case-insensitive on Windows, as Windows compares file names; exact on Linux. |
| `TASKSMACK_DETAILS_TAB` | `overview` (default), `gpu`, `npu` or `network`. `npu` selects the NPU tab only on a machine that has one. |
| `TASKSMACK_TAB` | The top-level tab to open, matched in this order: a tab's registered id (`SystemOverview`, `Processes`, `ProcessDetails`, `Services`, `Startup`, `SystemInfo`); one of the aliases `machine`/`overview` (the hostname tab), `details` and `system` (the System Information tab); or a tab's visible label (the hostname, the selected process's name). An id or an alias is never shadowed by another tab's label, so `system` opens System Information even while the machine tab still reads `System` -- its text until the first system sample brings the hostname (#2886); the hostname itself is matchable only once shown, so name the machine tab `machine` at startup. Case-insensitive (for ASCII only on Linux). Wins over the Details tab a selection opens; an unknown name logs one warning. The tab is selected by its id, not its position, and asked for until it shows (#1575). |
| `TASKSMACK_MACHINE_TAB` | The machine (hostname) tab's sub-tab to bring forward (#2869): `overview`, `cores` (CPU Cores), `gpu`, `npu` or `network` (Network and I/O). Case-insensitive; any other value logs one warning and is ignored. It picks the sub-tab only; the main tab stays `TASKSMACK_TAB`'s (or the default, the machine tab). The sub-tab is asked for until it shows, so a request made before the first sample (CPU Cores and Network and I/O exist only once it is in) still takes effect; one for a sub-tab this machine never draws (`npu` without an accelerator, `network` without network counters) is dropped with one warning after 120 frames drawn with a sample. `TASKSMACK_SCREENSHOT` does not wait for it: give `after` a few seconds, as the screenshot scripts do. |
| `TASKSMACK_OPEN` | `help` (the Help window), `about` (the About dialog) or `settings` (the Settings dialog, as the title bar's gear and F2 open it; #2827), opened at startup. Case-insensitive; any other value logs one warning. |
| `TASKSMACK_SCREENSHOT` | `<file.png>`, followed in the same value by any of `;after=<seconds>`, `;frames=<N>` and `;fps=<F>` (#2827). Once the tab and selection above have taken effect and `after` seconds (default 3, at most 60) have passed, writes a PNG of the window at its drawable size -- the pixels TaskSmack drew, read back in-app, so no desktop capture is involved -- and quits by the normal close path, so `config.toml` is saved. `frames` above 1 (at most 600) writes `<stem>-0001.png`, `<stem>-0002.png`, ... beside the given path instead, one every 1/`fps` seconds (default 10, 1 to 60) on the app clock, and quits after the last; a frame that renders late is captured as it is and the next keeps its scheduled time. A relative path is taken from the working directory. Logs `Screenshot written: <path> (WxH)` or `Screenshots written: N frames at F fps -> <stem>-NNNN.png (WxH)`. A read or write that fails is logged and the exit code is non-zero, as it is when TaskSmack closed before the capture. An invalid value logs one warning and is ignored; a number out of range is clamped with one. |
| `TASKSMACK_DEMO` | A comma-separated script of `<seconds>:<action>=<value>` steps (#2827), replayed from the moment the tab and selection above have taken effect -- the same point `TASKSMACK_SCREENSHOT` waits for, so with `after=0` a frame sequence records the script from its start. Each step is issued on the first frame at or after its time on the app clock, through the same in-process requests the variables above and the UI make; nothing is injected as input. Times have up to three decimals, must not decrease from step to step, and are at most 3600. Actions: `tab=<name>` (a main tab, as `TASKSMACK_TAB` names one); `select=name:<process name>` or `select=pid:<n>` (selects the row as a click does, when the process next appears in a snapshot; Process Details follows the selection but the main tab does not change); `details=<overview|gpu|npu|network>` (brings Process Details forward with that tab, as the row menu's **Details** does); `machine=<overview|cores|gpu|npu|network>` (brings the machine tab forward with that sub-tab, as `TASKSMACK_TAB=machine` with `TASKSMACK_MACHINE_TAB` does, #2869); `open=<help|about|settings>` (the Help window, the About dialog or the Settings dialog, as `TASKSMACK_OPEN` names them); `close=dialog` (closes the Help window, the About dialog and the Settings dialog, the last as **Cancel** does, saving nothing); `theme=<id>` (a loaded theme, applied as Settings applies one and saved with the settings); `section=<modules|openfiles|connections|environment|security|services>` (expands that section of Process Details' Overview the next time it is drawn, as a click on its header does; `openfiles` also starts the scan its **Scan** button starts, since the section lists nothing until then); `metrics=<on|off|export>` (#2923: `on` and `off` show and hide the Render Metrics overlay as **Ctrl+Shift+M** does, recording from that frame; `export` writes the overlay's CSV, the bytes its **Copy CSV** button puts on the clipboard for the last completed frame, to `TASKSMACK_RENDER_METRICS`' file, or with that unset to `<stem>-metrics.csv` beside `TASKSMACK_SCREENSHOT`'s path, and with neither set logs one warning and is skipped; an `export` with the overlay off writes the header only, with a warning). A step naming a tab or theme the app doesn't have logs one warning and is skipped; a value that doesn't parse logs one warning naming the step and the whole script is ignored. The hook's own tab and selection requests are never waited for by `TASKSMACK_SCREENSHOT`: its settle is latched once, and the script's requests live in slots of their own. Example: `TASKSMACK_DEMO="1:tab=processes,3:select=name:explorer.exe,5:details=network,8:theme=dracula"` with `TASKSMACK_SCREENSHOT=walk.png;after=0;frames=120;fps=12` records those 10 s as `walk-0001.png` to `walk-0120.png`. |
| `TASKSMACK_RENDER_METRICS` | A CSV file path (#2923). The Render Metrics overlay (**Ctrl+Shift+M**: per-chart vertices and CPU time, with ImGui's frame totals) is shown from the first frame, and at exit -- on the normal close path, where `config.toml` is saved -- the overlay's CSV for the last completed frame is written to the file: the same bytes its **Copy CSV** button puts on the clipboard (`timestamp_us,scenario,frame_id,publication_id,chart,vertices,cpu_us`, one row per chart drawn that frame). A relative path is taken from the working directory; the directory must exist, and an existing file is replaced. Logs `Render Metrics CSV written: <path> (N rows)`, or an error. With `TASKSMACK_SCREENSHOT`, one run leaves both the PNG and the CSV, the before/after artifact #843 asks for; a `TASKSMACK_DEMO` `metrics=export` step writes to the same file mid-run. The overlay's own cost is in the numbers (it draws and measures more), so compare runs that both have it on. |
| `TASKSMACK_CONFIG_DIR` | A directory (#1596) that replaces the config directory (`%APPDATA%\TaskSmack`, `$XDG_CONFIG_HOME/tasksmack` or `~/.config/tasksmack`) for `config.toml`, the single-instance lock beside it and the `themes` folder. Created if missing; a relative path is taken from the working directory. Logs one info line naming it. |
| `TASKSMACK_LOCALE` | A locale name (#1648), `de-DE` / `fr-FR` on Windows, `de_DE` / `fr_FR` on Linux (`.UTF-8` is tried first), that displayed numbers use instead of the user's regional format, so a capture on an en-US machine can show decimal commas. It changes only the user's locale `LocaleSetup` builds and the display punctuation, not the C locale or the global C++ locale. The startup log line ends in `(TASKSMACK_LOCALE)`; an unknown name logs one warning and is ignored. Unlike the variables above, it does not hide the "Limited Data" notice. |

**Every agent and developer test launch should set `TASKSMACK_CONFIG_DIR` to a fresh directory.**
Without it, a test instance shares the lock and `config.toml` with the TaskSmack you use every day:
whichever starts second is told "TaskSmack is already running" and exits (with code 75 once the box
is dismissed, so a script can tell it never ran), and a test instance that saves overwrites your settings. A per-run temp directory starts each run from default settings and
lets it run beside your own instance. (`tasksmack-debug.log` stays in the temp directory either way.)

Combined with `TASKSMACK_WINDOW` for a fixed size, `TASKSMACK_SCREENSHOT` writes the frame itself and
exits (no input, no desktop capture, and it works while the window is covered):

```powershell
$env:TASKSMACK_CONFIG_DIR = Join-Path $env:TEMP "tasksmack-test-$PID"
$env:TASKSMACK_WINDOW='1900x1000'; $env:TASKSMACK_SELECT_NAME='explorer.exe'; $env:TASKSMACK_DETAILS_TAB='overview'
$env:TASKSMACK_SCREENSHOT = "$env:TEMP\details.png;after=3"   # or "showcase.png;frames=30;fps=10" for an animation's frames
$p = Start-Process .\build\win-debug\bin\TaskSmack.exe -PassThru -Wait   # writes the PNG, then quits; $p.ExitCode is 0 once it did
Remove-Item -Recurse -Force $env:TASKSMACK_CONFIG_DIR
```

An animation of the app doing something -- a walk from the Processes table into Process Details, a
theme switch -- adds `TASKSMACK_DEMO`, whose steps play while the frames are captured:

```powershell
$env:TASKSMACK_DEMO = "1:tab=processes,3:select=name:explorer.exe,5:details=overview,7:details=network,9:theme=dracula"
$env:TASKSMACK_SCREENSHOT = "$env:TEMP\walk.png;after=0;frames=120;fps=12"   # the 10 s from the settle point, walk-0001.png ...
```

A running instance can still be captured from outside with `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT = 2)`,
which also works while the window is covered.

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

`tools/clang-tidy.sh` reads its compile database from one of three places (#2898):

- `debug` (the default) or `relwithdebinfo`: `build/<type>`, configured with `cmake --preset <type>` if it
  isn't yet, the stripped copy written to `build/<type>/clang-tidy-compdb/`.
- Any other configure preset (`./tools/clang-tidy.sh tsan`, `asan-ubsan`, `release`, ...): the
  `compile_commands.json` in the binaryDir that `CMakePresets.json` gives the preset. The script never configures
  it: a missing database is an error that names the `cmake --preset` command to run.
- `--build-dir DIR`: the `compile_commands.json` in any configured tree.

For a preset other than `debug`/`relwithdebinfo` or a `--build-dir`, the module and PCH flags are stripped into a
temporary copy that is removed when the run ends, so that tree's own files, including the `compile_commands.json`
clangd reads, are never touched. Every other option works the same with each, for example
`./tools/clang-tidy.sh tsan --changed-only`.

`.clang-tidy` sets `WarningsAsErrors: '*'`, so any clang-tidy finding fails the run, locally and in CI's
blocking jobs. Fix the finding, or suppress it with a `NOLINT(check-name)` comment that says why. Before
this, clang-tidy exited 0 on warnings, so the CI job could never fail on one (#1089). CI runs clang-tidy
twice, both blocking: on Linux over what `tools/clang-tidy.sh` analyses (which excludes
`src/Platform/Windows/**`), and on Windows over what `tools/clang-tidy.ps1` analyses (everything but
`src/Platform/Linux/**`, so the Windows platform code and the `_WIN32` branches of shared files) (#1233).
Each runs inside that OS's debug build job, after its tests and on the tree it configured (#1875 Windows,
#2047 Linux), so `src/Platform/Linux/**` and the `#ifndef _WIN32` branches are checked by the Linux run.
Neither run covers `tests/` translation units. Run the Windows script before pushing a change to Windows-only code. The Windows script skips one check,
`clang-analyzer-optin.core.EnumCastOutOfRange`, for an MSVC STL false positive it can't suppress in our code.

Both scripts take a result cache, `--cache-dir DIR` / `-CacheDir DIR` (#1785): a file that passed before with
identical inputs is skipped. `tools/tidy-cache.py` keys each file by its compile command, the content of every
file it includes (from `clang-scan-deps`), `.clang-tidy`, the script's flags and the clang-tidy build; only
clean results are recorded, and any cache problem means the file is analyzed. CI's tidy jobs use it (pull
requests restore main's cache, the nightly run saves it), and the Sunday run ignores it, so every file is
analyzed from scratch once a week. A nightly failure turns main red, which opens or updates main-health's
single `ci-red-main` issue; the job summary counts the findings by check.

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

CI heavy checks also publish a line-coverage summary and compare it with a floor (`LINUX_COVERAGE_THRESHOLD` and
`WINDOWS_COVERAGE_THRESHOLD` in `heavy-checks.yml`): Linux below 95% fails the coverage job, Windows below 85%
warns.

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

> **Note ("functions have mismatched data"):** `llvm-cov` warns that a couple of hundred functions
> have mismatched data. They are clang's unused-function placeholders: a header or inline function a
> binary references but never emits gets a hash-0 record with no counters, and when another binary
> ran that function the profile has it under that binary's real hash. The placeholder has nothing to
> lose, and the binary that ran the function carries the counts, so the report is complete.
> `tools/coverage.sh` confirms this on every run with `tools/coverage-mismatches.py`, and warns if a
> real record ever mismatches -- a function compiled differently in two binaries, whose counts would
> be missing (#1544). The check reads ELF sections, so `tools/coverage.ps1` doesn't run it.

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
> test. Linux CI runs `ctest` under `xvfb-run` with Mesa, and Windows CI on the runner's desktop with
> Mesa's llvmpipe `opengl32.dll` and `libgallium_wgl.dll` copied beside the test executables (the
> runners have no GPU, #2134); both set `TASKSMACK_REQUIRE_DISPLAY=1`, which turns a failed probe into
> a failure too, so a broken CI display can't hide as skips (#1132). On Windows these suites open
> windows on your desktop; set `TASKSMACK_SKIP_DISPLAY_TESTS=1` to skip them instead.

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

ClusterFuzzLite continuously exercises these parsers with libFuzzer and AddressSanitizer:

| Target | Entry point | Seed corpus |
|--------|-------------|-------------|
| `fuzz_proc_parsing` | the allocation-free `/proc` numeric parsers (`Platform/Linux/ProcParsing.h`) | none |
| `fuzz_smbios` | the SMBIOS table parser (`Platform/SmbiosParser.h`) | none |
| `fuzz_proc_maps` | the `/proc/[pid]/maps` module parser (`Platform/Linux/ProcMapsParser.h`) | `tests/fuzz/corpus/fuzz_proc_maps/` |
| `fuzz_proc_fd` | the `/proc/[pid]/fd` link classifier and `fdinfo` parser (`Platform/Linux/ProcFdParser.h`) | `tests/fuzz/corpus/fuzz_proc_fd/` |
| `fuzz_proc_status_security` | the `/proc/[pid]/status` security, `attr/current` and `cgroup` parsers (`Platform/Linux/ProcStatusSecurityParser.h`) | `tests/fuzz/corpus/fuzz_proc_status_security/` |
| `fuzz_commit_paging` | the Commit & paging parsers: `/proc/meminfo`, `/proc/swaps`, zram `mm_stat`, sysfs flags (`Platform/Linux/LinuxCommitPaging.h`) | `tests/fuzz/corpus/fuzz_commit_paging/` |
| `fuzz_mountinfo` | the Storage parsers: `/proc/self/mountinfo`, the volume choice, udev database files (`Platform/Linux/LinuxStorage.h`) | `tests/fuzz/corpus/fuzz_mountinfo/` |
| `fuzz_partition_layout` | the partition layout parsers: `DRIVE_LAYOUT_INFORMATION_EX` buffers, GPT type GUIDs and udev partition types (`Platform/PartitionTable.h`) | `tests/fuzz/corpus/fuzz_partition_layout/` |
| `fuzz_edid` | the monitor EDID parser: manufacturer, name and serial descriptors, image size (`Platform/EdidParser.h`) | `tests/fuzz/corpus/fuzz_edid/` |
| `fuzz_hwids` | the hwdata `pci.ids` / `usb.ids` parser: vendor, device and class lines (`Platform/HwIdsParser.h`) | `tests/fuzz/corpus/fuzz_hwids/` |
| `fuzz_proc_modules` | the `/proc/modules` parser: size, use count, dependents, `[permanent]`, state and taint flags (`Platform/Linux/LinuxKernelModules.h`) | `tests/fuzz/corpus/fuzz_proc_modules/` |
| `fuzz_coredump_names` | the systemd-coredump core file name parser: comm with `\xNN` escapes, uid, boot id, pid, time and compression (`Platform/Linux/LinuxCoredumps.h`) | `tests/fuzz/corpus/fuzz_coredump_names/` |
| `fuzz_user_config` | `App::UserConfig::parseSettings`: toml++ plus the `config.toml` schema, as `load()` reads it | `tests/fuzz/corpus/fuzz_user_config/` |
| `fuzz_theme_loader` | `UI::ThemeLoader::loadThemeFromString`: toml++ plus every theme colour lookup, as `loadTheme()` reads a file | `assets/themes/*.toml` and `tests/fuzz/corpus/fuzz_theme_loader/` |
| `fuzz_networkd_json` | systemd-networkd's link description: the JSON parser and the DHCPv4 lease read from it (`Platform/Linux/NetworkdLinkJson.h`) | `tests/fuzz/corpus/fuzz_networkd_json/` |
| `fuzz_dns_message` | the DNS reply parser for the opt-in public IP lookup, the only parser fed bytes from the network (`Platform/DnsMessage.h`) | `tests/fuzz/corpus/fuzz_dns_message/` |
| `fuzz_desktop_entry` | the `.desktop` entry parser and its `Exec` splitting (`Platform/Linux/DesktopEntryIcons.h`) | `tests/fuzz/corpus/fuzz_desktop_entry/` |
| `fuzz_broker_protocol` | the privileged helper's request decoder, which reads what an unprivileged process sends (`Platform/BrokerProtocol.h`) | `tests/fuzz/corpus/fuzz_broker_protocol/` |
| `fuzz_broker_socket` | the privileged helper's frame reassembly, which joins an unprivileged client's packets by the frame's length prefix over a real socketpair (`Platform/Linux/BrokerSocket.cpp`) | `tests/fuzz/corpus/fuzz_broker_socket/` |

Pull requests that change `tests/fuzz/**`, `.clusterfuzzlite/**`, the `cflite_*.yml` workflows,
any fuzzed parser (the headers in the table above, `UserConfig.{cpp,h}`, `UserConfigHelpers.h`,
`ThemeLoader.{cpp,h}`, `BrokerSocket.{cpp,h}`, and the types they fill, such as `Theme.h` and `ProcessColumnConfig.h`), the fuzz build's other inputs
(`assets/themes/**`, the theme seed corpus, and `tests/Mocks/ThemeStub.cpp`), or the dependency
pins and patches (`cmake/Dependencies.cmake`, `cmake/patches/**`) run a short
code-change fuzzing job (`cflite_pr.yml`'s path filter); pushes to `main` touching the same paths
refresh the baseline build (`cflite_build.yml`). The two lists are kept by hand, since a `paths:` filter
can't be shared between workflows; when a new target adds a parser, add its paths to both, and the
`check-cflite-paths` pre-commit hook fails if they differ (#2533). Separate weekly jobs perform a longer batch run and prune the resulting corpus.
The weekly batch run (`cflite_batch.yml`) shares 900 seconds across every target, under a minute each.
A release candidate's campaign gives each target 600 seconds instead (#2783):
`gh workflow run cflite_batch.yml --ref <candidate branch> -f campaign=release`, then quote the run's
summary line (targets, total and per-target seconds) in the release's fuzzing record.
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

For repeatable benchmarking runs (recommended), use the helper scripts. They run every benchmark 10
times, which takes a while: about 25 minutes on a 16-core x64 machine (#2618). Pass a
`--benchmark_filter=` to run fewer.

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

For the script tests, the `TASKSMACK_BENCH_BIN` / `TASKSMACK_BENCH_OUT_DIR` environment variables
point either script at a stub binary and a scratch directory (`tools/test-bench.ps1`,
`tests/tools/test_bench_sh.py`); `bench.ps1`'s `-BenchmarkBinary <path>` / `-OutputDirectory <dir>`
do the same and win over the variables.
A relative binary path, a bare name included, is relative to the current directory (the
PowerShell location for `bench.ps1`) and is never looked up on `PATH`: the file that is checked
and hashed is the one that runs. They are also the only way to move the output: an extra `--benchmark_out` or
`--benchmark_out_format` argument is refused (exit code 2) before the benchmark starts, because the redaction
and the manifest only look at the file the script chose. A benchmark that fails or crashes makes
either script exit with the benchmark's own exit code. `bench.sh` runs its Python helpers with the
interpreter `find_python` in `tools/common.sh` picks (the project `.venv`, else Python 3.14+ on
`PATH`; a Windows `.venv\Scripts\python.exe` only from Git Bash, never from WSL). `./tools/bench.sh --help` and `Get-Help tools/bench.ps1 -Full` describe the options.

By default, benchmarks output to console. You can also:

```bash
# Keep baselines platform-specific for apples-to-apples comparisons
# Linux baseline:    perf-data/linux-baseline.json
# Windows baseline:  perf-data/win-baseline.json

# Compare Windows runs (median per benchmark; exits 1 on a regression over --threshold, default 15%)
python tools/check-benchmark-regression.py --baseline perf-data/win-baseline.json --current perf-data/win-benchmark-<timestamp>.json

# Compare Linux runs
python3 tools/check-benchmark-regression.py --baseline perf-data/linux-baseline.json --current perf-data/benchmark-<timestamp>.json
```

### CI Benchmark Regression Gate

PR CI (`ci.yml`'s Linux Release job) only *builds* `TaskSmackBenchmarks` -- it never runs it -- so a
change that breaks the benchmark build fails the PR (#1348). Timing runs and the regression gate below
live only in `heavy-checks.yml`.

`heavy-checks.yml`'s `benchmark-regression` job runs nightly and on pushes to `main` that touch the code the
benchmarks measure, gating against
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

This is a *separate* baseline from `perf-data/linux-baseline.json` above, deliberately: that one was
recorded on a developer machine, and comparing it with hosted `ubuntu-24.04` runners flagged the machine
difference as a regression on every run (#683). Hosted runners also land on two speed classes, so the
committed CI baseline is a capture from the slower class: a run on the faster class can only look like an
improvement, never a false regression (#682). The two classes don't slow every benchmark alike (#2336):
allocator- and memory-bound benchmarks vary more, so `--threshold-overrides FILE` gives the benchmarks a
JSON file lists their own threshold, each with a required `reason`. `perf-data/linux-ci-thresholds.json`
sets those. Add an entry only with the captures that justify it, and remove it when a baseline refresh
makes it unnecessary. Without the flag every benchmark uses `--threshold`.

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
   If every benchmark shifted by roughly the same percentage in the same direction, that's very
   likely a runner-class difference, not a real change: dispatch one more run and compare before
   deciding which to keep.
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
| `BM_DiskProbe_Read` | Raw OS disk I/O counter read |
| `BM_ProcessTree_*`, `BM_ProcessFilter_Indices` | Building the Processes table's tree-view rows, and the filter pass over Windows-like command lines |
| `BM_ProcessRowMeter_*` | One headless ImGui frame of the Processes table with and without the inline meters (#1528) |
| `BM_ProcessIcons_*` | One headless ImGui frame of the Processes table's name column with and without process icons (#1800) |
| `BM_ProcessDetailsHistory_*` | Process Details' per-process history append and trim at a full window (#1179) |
| `BM_PDHGPUProbe_*`, `BM_PDHInstanceCache_*` | The PDH per-process GPU counter read and its instance-name cache (Windows only) |
| `BM_Format_*` | UI formatting functions |
| `BM_NetlinkSocketStats_*` | Netlink INET_DIAG socket query performance (Linux only) |
| `BM_StorageModel_*` | Storage probe/model sampling, history accessor, and per-disk snapshot performance |
| `BM_StorageModel_MemoryGrowth` | Memory growth over repeated `sample()` cycles |
| `BM_GPUProbe_*`, `BM_GPUModel_*` | GPU probe enumeration, counter reads, model refresh, and history accessor performance (skipped on a machine with no GPU, #1420; the mock-probe `BM_GPUModel_History_Publish`/`BM_GPUModel_Concurrent_PublicationWait` below always run) |
| `BM_GPUModel_MemoryGrowth` | Memory growth over repeated GPU `refresh()` cycles |
| `BM_Numeric_*` | Micro-benchmarks for `toDouble`, `clampPercentToFloat`, `narrowOr`, and mixed process-table workload |
| `BM_ChartWidgets_*` | `UI::Widgets` chart helpers: `computeAlpha` smoothing, `tailAlignedSpan` history-window selection, and the `formatAxisLocalized`/`formatAxisBytesPerSec` axis-label formatters — the layer the Windows ETW app-trace (perf-plan-574 / issue #574) flagged as expensive but that previously had no Linux-runnable coverage |
| `BM_ChartGeometry_*` | One whole headless ImGui+ImPlot frame (`NewFrame()` through `Render()`, no window or GL) of the real `ChartWidgets.h` charts with fixed data: the stacked CPU chart at full history, the same chart over the default window in a 480 px window (where its point budget follows the plot width, #843), the per-core grid (16 cores, and 64 narrow ones whose point budget follows the plot width, #1411), the memory chart, and an uncached min/max-reduced 18k-sample line. Reports `vertices`/`indices`/`draw_lists`/`draw_cmds` counters; the same scenes (`benchmarks/ChartGeometryScenes.h`) are held to a vertex/index budget by `tests/UI/test_ChartGeometryBudget.cpp`, which gates every PR (#1421) |
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

Every app capture, with `profile-perf.sh` and `profile-heap.sh` alike, runs TaskSmack with a fresh,
empty `TASKSMACK_CONFIG_DIR` that is removed afterwards (#2250). The run starts from default settings,
never rewrites yours, and is not stopped by the single-instance lock of a TaskSmack you already have
open. Pass `--user-config` to profile with your own settings instead; the run then refuses to start
while a TaskSmack of yours is running. A TaskSmack turned away by the lock (it logs "TaskSmack is
already running" or exits with code 75) fails the run instead of being profiled.

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

# App trace (fresh temporary config; --user-config profiles with your own settings)
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
scripts build before prompting for elevation. App mode runs TaskSmack with a fresh, temporary
`TASKSMACK_CONFIG_DIR`, removed afterwards and recorded in the manifest, so a TaskSmack you already
have open neither blocks the capture nor has its settings touched (#2250). `-UseUserConfig` profiles
with your own settings instead and refuses to start while TaskSmack is running. An app that exits
with the "already running" code (75) fails the capture, and `resize` mode's App phase uses a
`config` folder in its run directory. For resize diagnosis, use the `resize`
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
- `wpr` comes with Windows, so `profile-etw.ps1` records on any machine. `analyze-etw.ps1` also
  needs `xperf` (and `wpa`/`wpaexporter` to browse or export a trace). Those come with the Windows
  Performance Toolkit, which `tools/setup-dev.ps1` does not install. Get it from the
  [Windows SDK installer](https://developer.microsoft.com/windows/downloads/windows-sdk/) by
  selecting only **Windows Performance Toolkit**; it installs to
  `C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit` (add that folder to `PATH`
  if a new terminal doesn't find `xperf`). `check-prereqs.ps1` shows these rows as optional
  (#2618).
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
- A capture never overwrites an earlier one (#2254): two runs in the same second get `-2`, `-3`, ...
  appended to the timestamp, and an explicit `-Timestamp` an earlier capture in the output directory
  already used is refused.
- Without `-SymbolPath`, `analyze-etw.ps1` decodes against `build\<preset>\bin` for the preset the
  trace's `<trace>.manifest.json` records (`win-release` for a default app capture), so the symbol
  identity check compares the build that was captured; with no manifest it uses `win-profile`.
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
  draws and measures more. Turn it on only to compare its readout with PresentMon's. For that
  comparison without input, `TASKSMACK_RENDER_METRICS=<file.csv>` shows the overlay from the first
  frame and writes its CSV at exit (see "Visual validation without input", #2923).

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
$env:TASKSMACK_TRACE_RESIZE_PERF=1; .\build\win-profile\bin\TaskSmack.exe 2>&1 | Tee-Object $env:TEMP\resize-trace.log
```

Just let the app sit idle for a few seconds to capture steady-state numbers, and/or resize the
window (edges and corners) for 20–30 seconds to capture interactive numbers, then close the app.
The log contains `ResizePerf[idle-progress|idle-end|interaction-progress|interaction-end|shutdown]` lines.
Idle frames log every 5 s (`IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS`); interaction frames log every
0.5 s (`RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS`) since an interaction is a short, bounded burst
where more frequent logging is useful. Each line reports avg/p95/p99/max per phase, not just
avg/max — p95/p99 answer "did we miss the 16.6 ms (60 fps) frame budget at the tail", which an
average can hide and a single max spike can overstate:

```text
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

On Windows, `tools/measure-idle.ps1` (#2492) takes one measurement of an existing build the same way:
its own fresh `TASKSMACK_CONFIG_DIR`, `TASKSMACK_TRACE_RESIZE_PERF=1`, a warm-up, then per-thread
CPU (from each thread's processor time), fps and frame times from the same `ResizePerf` summaries,
and the private working set (Task Manager's "Memory"), private bytes and working set at the start
and end. It closes the app with `WM_CLOSE` and writes a table, a `RESULT` line and JSON. It has no
`--repeat` or `--fail-above`; run it again for another sample. Windows numbers pick up whatever
else the machine is doing, so compare runs taken back to back. `-RedirectAppData` measures a build
older than `TASKSMACK_CONFIG_DIR` (v0.8.0) without touching your own `%APPDATA%\TaskSmack`; give it
a `config.toml` with `[window]` `width`/`height` there, since such a build ignores `-Window` too.

```powershell
pwsh tools/measure-idle.ps1 -Preset win-release -Window 1280x720 -Warmup 60 -Duration 300
Get-Help tools/measure-idle.ps1 -Full
```

Per-thread CPU comes from `pidstat -u -t -p <pid> 1 <N>` (package `sysstat`) when installed,
otherwise from `/proc/<pid>/task/*/stat` deltas over the same window. Threads are named so the rows
are readable: the background samplers are `ts-sampler-proc` (process enumeration),
`ts-sampler-sys` (CPU, memory, network, power), `ts-sampler-disk` (disks) and `ts-sampler-gpu`
(GPUs and NPUs; one thread per domain, #2731), plus `ts-sampler-svc` (the Services tab's list, created
the first time that tab is shown) and `ts-sampler-auto` (the Startup tab's list, while it's shown). On Linux the UI thread keeps the process name (`TaskSmack`;
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
or fix. The targets cover **Linux/WSL app CPU only**; there is no Windows capture script yet, so Windows has
no targets. Re-measure and revisit them when the renderer, the default
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

### PGO builds are not profiling builds

Two families of presets have "profile" in their purpose; keep them apart, and never time one as the other:

| Preset (Linux / Windows) | Output dir | What it is for |
|---|---|---|
| `pgo-generate` / `win-pgo-generate` ("PGO Training") | `build/<preset>` | **Instrumented** (`-fprofile-instr-generate`): counts branches to write `.profraw` training data. Slow; never use it for timing or flamegraphs. |
| `pgo-use` / `win-pgo-use` | `build/<preset>` | The PGO-optimized build: `-O3 -march=x86-64-v3 -fprofile-instr-use=profiles/tasksmack.profdata`. |
| `pgo-baseline` / `win-pgo-baseline` | `build/<preset>` | `pgo-use`'s flags **without** profile data: the build to measure PGO against. |
| `profile` / `win-profile` ("sampling profiler, not PGO") | `build/<preset>` | A plain profiling build (`-O2 -g`, frame pointers) for perf, ETW and VTune. No PGO instrumentation or profile data. |

The scripts' banners say which kind each phase builds.

### How it works

1. **Build an instrumented binary** (`pgo-generate`/`win-pgo-generate` preset) with `-fprofile-instr-generate`
2. **Run the training workloads** to collect branch-count data into `.profraw` files (one per process, `%p`):
   - the benchmark suite (`TaskSmackBenchmarks`), for the Domain and Platform hot paths;
   - the **headless UI training driver** (`TaskSmackUiTraining`, #880), for the rendering paths: TaskSmack's real panels, every main tab, drawn by ImGui/ImPlot for a fixed number of frames with no window and no GPU, once on the real probes and once on the synthetic large-UI machine (#1413).
3. **Merge** the `.profraw` files into a single `.profdata` with `llvm-profdata`
4. **Build the optimized binary** (`pgo-use`/`win-pgo-use` preset) with `-fprofile-instr-use=<path>.profdata`

### The UI training workload

`TaskSmackUiTraining` (`src/Training/`) builds the same six panels `ShellLayer` does, on the app's own object files (the `TaskSmackApp` object library, so its profile is for exactly the code `TaskSmack` links), and draws them in the same tab bar. It is a separate executable: the shipping app has no training switch. Each run:

- renders every workload below, in order, each for its share of the frames (default 1800 frames, paced at 60 fps: about 30 s per run, so about a minute for both probe sets);
- starts from the default settings in a fresh temporary `TASKSMACK_CONFIG_DIR`, removed at exit, so it never reads or writes your `config.toml`;
- gives ImGui no input at all, so no button is ever pressed: it never ends, kills, suspends or re-prioritizes a process, or touches a service or startup item;
- checks that every workload brought its tab forward, logs the frames and vertices it drew per workload, and exits 0 so the instrumented build writes its profile.

The weights are percentages of a run's frames, in `src/Training/UiTrainingPlan.h`:

| Workload | Tab | Weight | Why |
|---|---|---:|---|
| `overview` | System Overview | 25 | The default tab: every history chart and the NowBars |
| `processes` | Processes (flat list) | 18 | The biggest table, sorted, with row meters |
| `processes-tree` | Processes (tree view) | 10 | The tree flatten and indent paths |
| `details-overview` | Process Details, Overview | 14 | The selected process's charts and actions block |
| `details-gpu` | Process Details, GPU | 4 | The per-process GPU charts |
| `details-network` | Process Details, Network | 6 | The connections table |
| `services` | Services | 8 | The services table (sampled only while shown) |
| `startup` | Startup | 7 | The startup items table |
| `system` | System (information) | 8 | The static facts page |

Process Details shows the driver's own process with the real probes, and the synthetic machine's busiest long-lived process with the synthetic ones, replaced by a live one if it exits. A Details workload fails the run if it never drew a running process, only the "Process exited" or "not found" pane. Run it by hand to see the plan or to train with other settings:

```bash
./build/pgo-generate/bin/TaskSmackUiTraining --list                       # the plan, then exit
LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
    ./build/pgo-generate/bin/TaskSmackUiTraining --synthetic processes=10000,history=full --frames 3600
```

`--probes real|synthetic`, `--synthetic SPEC` (the `TASKSMACK_SYNTHETIC` syntax), `--frames N` and `--fps N` (0 renders back to back) are its options. It is built by every preset (`TASKSMACK_BUILD_UI_TRAINING`, ON by default), and the `UiTrainingSmokeReal` and `UiTrainingSmokeSynthetic` ctest tests run it for 100 unpaced frames, without the PGO toolchain.

### Automated workflow (recommended)

Use the provided helper scripts to run all three phases:

```bash
# Linux – full workflow (build instrumented, run benchmarks + UI training, merge, build optimized)
./tools/pgo.sh

# Run individual phases
./tools/pgo.sh generate   # Phase 1: instrumented build + profile collection (benchmarks + UI training)
./tools/pgo.sh merge      # Phase 2: merge *.profraw → profiles/tasksmack.profdata
./tools/pgo.sh use        # Phase 3: build PGO-optimized binary
./tools/pgo.sh baseline   # pgo-use's flags without profile data, to compare against

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
pwsh tools/pgo.ps1 baseline

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

# The UI training workload: once per probe set
for probes in real synthetic; do
    LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
        ./build/pgo-generate/bin/TaskSmackUiTraining --probes "$probes"
done

# Optionally add an interactive session on top (it does not replace the training above)
LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
    ./build/pgo-generate/bin/TaskSmack

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

### Measuring PGO

Judge a PGO claim, especially a rendering one, only between builds that differ in nothing but the profile data and its flag: `pgo-use` against `pgo-baseline` (`win-pgo-use` against `win-pgo-baseline`). Both are `Release` with `-O3 -DNDEBUG -march=x86-64-v3` and the same options, from the same commit; only `-fprofile-instr-use` differs. Comparing `benchmark` (or `release`) with `pgo-use` measures PGO and `-march=x86-64-v3` together, and anything timed on `pgo-generate` measures the instrumentation.

To compare two sets of training data (say, benchmarks only against benchmarks plus UI training), build `pgo-use` from each `.profdata` in turn, from the same commit, and compare those two builds the same way.

```bash
# Baseline: pgo-use's flags, no profile data
./tools/pgo.sh baseline
./build/pgo-baseline/bin/TaskSmackBenchmarks --benchmark_format=json > /tmp/baseline.json

# PGO (after ./tools/pgo.sh)
./build/pgo-use/bin/TaskSmackBenchmarks --benchmark_format=json > /tmp/pgo.json

# Compare (prints each benchmark's change; without repetitions each single run is compared)
python3 tools/check-benchmark-regression.py --baseline /tmp/baseline.json --current /tmp/pgo.json
```

For rendering, run the same UI scenario on both builds: the synthetic large-UI machine (`TASKSMACK_SYNTHETIC=processes=5000,history=full`) with a fixed `TASKSMACK_WINDOW` and `TASKSMACK_TAB`, compared through the frame-time tooling (`tools/profile-etw.ps1` on Windows).

### Tips

- **Re-profile after large refactors.** Stale profile data still improves performance, but fresh data gives the best results.
- **Change the UI weights in one place.** `src/Training/UiTrainingPlan.h` holds the table above; its unit tests check the weights add up to 100 and that every main tab is trained.
- **Keep the driver in step with the shell.** `TrainingShell` (`src/Training/UiTrainingDriver.cpp`) mirrors `ShellLayer`'s attach, update and tab rendering; a new main tab or a new per-frame shell step belongs in both.

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

What a package contains, and what it depends on (#1398):

- Only the executable, the runtime assets, the Linux desktop entry and icons, the privileged helpers
  described below, and the licences (`LICENSE`, plus the third-party licences under `share/doc/tasksmack/third_party/` on Linux or
  `third_party_licenses/` in the Windows `.zip`). Every library TaskSmack is built from is linked
  statically, so no headers, `.a`/`.lib` files, or CMake/pkg-config files belong in a package.
  Whatever the FetchContent dependencies' own install rules declare goes into a `third_party`
  component (`tests`/`benchmarks` for the test and benchmark dependencies), and CPack packages
  only the default component, TaskSmack's own rules in `cmake/InstallRules.cmake`.
- Linux Release builds link libc++ and libc++abi statically (`TASKSMACK_STATIC_LIBCXX`, ON by
  default for `CMAKE_BUILD_TYPE=Release`), because the distro's `libc++1` is older than the
  apt.llvm.org clang the release is built with. `readelf -d build/release/bin/TaskSmack` should
  list no `libc++*.so`. Debug and the sanitizer presets keep the shared libc++.
- The `.deb`'s `Depends` come from `dpkg-shlibdeps`, plus the libraries SDL3 `dlopen()`s that it
  can't see (X11, GL/EGL), listed in `cmake/Packaging.cmake`; Wayland, the other X11 extensions,
  Mesa's drivers and `pkexec | policykit-1` (for the helper below) are `Recommends`.
- Linux's privileged helper, `tasksmack-helper` (#2307), and its polkit action are the `helper`
  install component: the helper goes to `${CMAKE_INSTALL_LIBEXECDIR}/tasksmack/` and
  `assets/linux/app.tasksmack.TaskSmack.helper.policy` to `${CMAKE_INSTALL_DATADIR}/polkit-1/actions/`
  (`cmake/InstallRules.cmake`). Only the `.deb` and `.rpm` package that component
  (`cmake/CPackProjectConfig.cmake.in`); the `.tar.gz` doesn't, and TaskSmack then offers no
  **Authenticate and retry** / **Authenticate to read**. TaskSmack starts the helper with `pkexec` from
  the fixed path `/usr/libexec/tasksmack/tasksmack-helper` (`PkexecBroker::INSTALLED_HELPER_PATH`),
  which is also the policy's `org.freedesktop.policykit.exec.path`, so a package must use the `/usr`
  prefix and the default `libexec` directory. The action is `app.tasksmack.TaskSmack.helper`, with
  `auth_admin_keep` for an active session and `auth_admin` otherwise. The helper links only Platform
  code (no SDL, ImGui, GL or config), and TaskSmack doesn't offer it when it already runs as root.
- Windows' elevated helper, `TaskSmackHelper.exe` (#2550), installs beside `TaskSmack.exe`
  (`cmake --install` always installs it, in the `windows_helper` component). The `.zip` and the NSIS
  installer carry it only with `-DTASKSMACK_PACKAGE_WINDOWS_HELPER=ON`, which is for
  Authenticode-signed builds (#2302). Packaging then runs `cmake/VerifyWindowsSignatures.cmake` first,
  which refuses unless `TaskSmack.exe` and the helper both have valid, timestamped signatures from a
  certificate whose common name is `TASKSMACK_EXPECTED_SIGNER` (default `Matt Gradwohl`, the Azure
  Artifact Signing profile's subject, #2302), the same subject for both.
  Off (the default), unsigned dev builds package as before, without the helper. Sign both
  executables in `build/<preset>/bin` after building and before `cpack`, or, as the release does,
  sign the packaged `.zip` instead: `-DTASKSMACK_SIGN_AFTER_PACKAGING=ON` moves the check off the
  `.zip`'s CPack run, and `cmake -DTASKSMACK_VERIFY_DIR=<extracted .zip>
  -DTASKSMACK_EXPECTED_SIGNER=<CN> -P cmake/VerifyWindowsSignatures.cmake` must then pass on the
  signed `.zip` before it's published (see [Windows code signing](#windows-code-signing)). The NSIS
  installer keeps the check either way, since nothing can sign inside it afterwards. The helper serves only an
  install its user can't change (`BrokerHelper::checkUnchangeableBy`), so it works from the NSIS
  installer's per-machine `C:\Program Files\TaskSmack`, but not from a `.zip` unpacked into a folder
  its user owns.
- Windows builds other than Debug link the C and C++ runtime statically (`/MT`), so the `.zip`'s
  `TaskSmack.exe` runs without the Visual C++ Redistributable: `llvm-objdump -p` on it should list
  no `MSVCP140.dll`, `VCRUNTIME140*.dll` or `api-ms-win-crt-*.dll`. Debug keeps `/MDd`.

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
| `TASKSMACK_ENABLE_GHASH` | `OFF` | Windows clang Debug: `-gcodeview-ghash` plus lld-link `/DEBUG:GHASH`, faster PDB links (CI's Windows debug build turns it on) |
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

Clang-tidy configuration is curated for signal/noise; see `.clang-tidy` for the current list of disabled checks.

## Adding Dependencies

Use CMake’s `FetchContent` for dependencies. Declare new dependencies in `cmake/Dependencies.cmake`, always use `SYSTEM` to suppress third-party warnings, and always pass a per-preset `BINARY_DIR` and a per-host `SUBBUILD_DIR` (see "Shared FetchContent cache" below):

```cmake
FetchContent_Declare(
    mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/mylib-build"
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/mylib-subbuild"
)
FetchContent_MakeAvailable(mylib)

target_link_libraries(TaskSmack PRIVATE mylib)
```

### Shared FetchContent cache

The shared FetchContent cache is **enabled by default** to reuse downloads across presets, reducing build times and bandwidth usage. The cache is stored at `.cache/fetchcontent/` in the project root.

Only the downloads are shared: `.cache/fetchcontent/` holds each dependency's `<dep>-src`, while its
`<dep>-build` tree lives in the preset's own `build/<preset>/_deps/`, so a sanitizer, coverage or LTO
preset never reuses objects another preset compiled with different flags (#1308). Configuring fails if
any added directory builds outside the preset's build tree -- that is the check that a new
`FetchContent_Declare()` without `BINARY_DIR` trips.

The `<dep>-subbuild` (the small project that clones `<dep>-src`, plus its download stamps) is per host:
`.cache/fetchcontent/subbuild/<OS>-<hash>/`, keyed by host OS, the cache path as that host spells it and
the generator, so one checkout configures from both WSL (`/mnt/c/...`) and Windows (#2253). Every
`FetchContent_Declare()` passes `SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/<dep>-subbuild"`, and
configuring fails if a sub-build lands directly in `.cache/fetchcontent/`. The download stamps record the
host's own paths, so the first configure on a second host clones the sources again. To reuse the ones
the other host already fetched, configure that host's presets with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`
(it stays set in that build tree; set it back to `OFF` when a dependency pin changes).

To disable the cache:

```bash
cmake --preset debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
cmake --preset win-debug -DTASKSMACK_ENABLE_FETCHCONTENT_CACHE=OFF
```

Override the cache dir with `TASKSMACK_FETCHCONTENT_CACHE_DIR` or `FETCHCONTENT_BASE_DIR`.

## CI/CD

We use GitHub Actions for our CI workflows. They are categorized as follows:

### Core Build & Test
- **`ci.yml`**: The primary hub. Runs on pushes and PRs to `main` and `release/*`, nightly (Monday to Saturday; the Sunday weekly run is the same full run plus the unity build), and via manual dispatch.
  - **Builds:** Linux and Windows Debug on push/PR; Linux Release on the same events plus the nightly schedule, also compiling and linking (but not running) `TaskSmackBenchmarks` so a PR that breaks the benchmark build fails CI (#1348); Windows Release nightly and on dispatch only; the unity build weekly. Docs-only pull requests skip the C++ builds and `clang-tidy`. It also checks Markdown links.
  - **`clang-tidy` (blocking):** runs on Linux and Windows on PRs, the schedule and dispatch, not on pushes to `main`. The Windows run is skipped when every change is Linux-only, and the Linux run when every change is Windows-only. Each run happens inside that OS's debug job, after its tests and on the tree it configured, so LLVM setup and configure happen once per OS per PR head (the nightly runs those jobs in clang-tidy-only mode, without a build). The tidy step is `continue-on-error`, so a finding leaves the build check green and is reported separately: a small `Static Analysis (Windows)` job reports the Windows result under the check name the ruleset requires (#1875), and `CI Success` fails on a Linux finding or on a Linux tidy run that didn't happen, with a `clang-tidy (Linux)` annotation (#2047).
  - **Which files:** on a PR it analyzes only the translation units the change can affect (changed `src/` `.cpp` files plus every `.cpp` that includes a changed `src/` header), and falls back to every file when `.clang-tidy`, the tidy scripts, CMake files, `ci.yml`, the toolchain setup actions (`setup-llvm`, `setup-windows-llvm`, `setup-python-glad`) or `requirements-glad.*` change (`tools/tidy-changed-files.py`, #1406). The nightly run analyzes every file.
  - **Other jobs:** a blocking Address/Undefined Behavior sanitizer job (`ASan+UBSan`, part of `CI Success`) on PRs, and IWYU (include analysis) on manual dispatch only.
  - **`CI Success`** is the gate job used for branch protection. When a needed job was cancelled (a superseded push or a manual stop) the gate still fails, but its first step reports "CANCELLED, not a code failure" so it isn't mistaken for a broken build.
- **`reusable-build-test.yml`**: Contains the actual matrix steps for setting up LLVM, Python, `ccache`, configuring CMake, building, and running CTest tests, plus an optional Linux build-only `TaskSmackBenchmarks` step (`build_benchmarks` input). Called by other workflows. It configures with `-DTASKSMACK_ENABLE_PCH=OFF` (except the weekly unity build) because ccache can't cache PCH-using compiles; local presets keep PCH on.
- **`arm64.yml`**: Linux aarch64 (`ubuntu-24.04-arm`) and Windows ARM64 (`windows-11-arm`, plain ARM64, not ARM64EC) debug build and full `ctest`, through `reusable-build-test.yml` (#2214). It runs on pushes to `main` and `release/*`, nightly and on manual dispatch, and on a pull request only when the PR carries the `ci:arm64` label (adding the label starts a run). It is not part of `CI Success` or any required check, and without the label its jobs are skipped before a runner is assigned. Windows ARM64 installs the native `LLVM-<ver>-woa64.exe` (`setup-windows-llvm`, pinned by SHA-256; Chocolatey only has the x64 installer) and runs the display suites on `mmozeiko/build-mesa`'s ARM64 llvmpipe (`setup-mesa-llvmpipe`, pinned by SHA-256, test-only). `release.yml` builds and ships the ARM64 packages with its own matrix; this workflow is the PR and nightly test leg. On `windows-11-arm` the PowerShell script tests (`pwsh-script` label) run in a second `ctest` with `--repeat until-pass:2`, because pwsh itself crashes there now and then ("Internal CLR error", #2832); nothing else is retried.
- **`manual-build.yml`**: Manual dispatch entry point to trigger a specific OS and build type build from the GitHub UI without opening a PR.

CI caches (#1406): ccache and the FetchContent source cache (`.github/actions/fetchcontent-cache`) are **saved only by runs on `main`** in every workflow, `release.yml` included (tag runs only restore); pull requests restore main's entries and never save their own, which kept the repository under its 10 GB Actions cache quota. Runs on a `release/*` branch save none either, so its pull requests read `main`'s entries (the default branch's), like every other PR. One job per OS saves the FetchContent cache: Windows debug, and the Linux release build after its benchmark-enabled configure, so the saved sources include Google Benchmark. The FetchContent key hashes only what declares or patches dependencies: `cmake/Dependencies.cmake`, `cmake/patches/**`, and the `FetchContent_Declare(...)` blocks of `tests/CMakeLists.txt` and `benchmarks/CMakeLists.txt` (not those whole files, which change whenever a test is added, #1582). `cache-prune.yml` runs after each `main` run of a cache-saving workflow (listed under `cache-prune.yml` below) and deletes all but the newest entry of each ccache and FetchContent family, so `main` stays under the quota and least-recently-used eviction never removes a live entry (#1583). clang-tidy doesn't compile through ccache; the debug jobs' tidy runs simply reuse the build's configured tree, and `static-analysis.yml` configures with the same flags (`TASKSMACK_ENABLE_PCH=OFF`, plus the Windows flags below) so every run computes the same result-cache keys. The Windows debug build configures with `-DTASKSMACK_ENABLE_IPO=OFF -DTASKSMACK_ENABLE_GHASH=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF` (CI only; the presets keep ThinLTO in Debug), which cut its five links from about 50-70 s each to 1-5 s and drops ~530 `clang-scan-deps` runs per build. Its ccache is 750 MB (500 MB elsewhere) with `inode_cache` on, and every build job's post step prints `ccache -s -vv` (hits, misses and why calls were uncacheable); with `TASKSMACK_ENABLE_PCH=OFF`, as CI configures, the dependencies' own precompiled headers (SDL3's) are off too, since ccache can't cache a compile that uses one (#1875). Each build job's summary also gets a build profile from `.ninja_log` (`tools/ninja-build-profile.py`): time per edge kind and the slowest links.

### Security & Fuzzing
- **`codeql.yml`**: Runs GitHub's CodeQL engine to trace execution and analyze the C/C++ codebase for semantic security vulnerabilities (pushes/PRs to `main` and `release/*`, weekly). It traces only the app's own compiles (TaskSmack's objects; the tests, the PGO UI training driver and the third-party libraries are not extracted), and its required `Analyze C++` check is skipped, which passes it, for docs-only pull requests (#1786).
- **`osv-scanner.yml`**: Uses Google's OSV-Scanner to check dependencies against the Open Source Vulnerability database (pushes/PRs to `main` and `release/*`, weekly, manual dispatch).
- **`renovate.yml`**: Self-hosted [Renovate](https://docs.renovatebot.com/) run, scoped to C++ `FetchContent` libraries and the build/dev toolchain (LLVM, Python, CMake, Ninja, ccache, pre-commit's own hook tools) -- the freshness gap Dependabot/OSV-Scanner don't cover (weekly, manual dispatch with dry-run options). See "Keeping Dependencies Current" below.
- **`scorecard.yml`**: Evaluates the repository against OpenSSF security best practices (branch protection, pinned dependencies) and uploads results to the security dashboard (weekly, on branch-protection changes, and manual dispatch). Its SAST check counts a merged PR as scanned only if a code-scanning check run (GitHub Advanced Security's `CodeQL` or `osv-scanner`) has completed on the PR's head commit when Scorecard runs. It used to run on every push to `main`, seconds after the merge, which scored a PR merged before its CodeQL finished as unscanned (#1405); the weekly run sees those results long after they land (#1406).
- **`dependency-review.yml`**: Scans PRs to block any that introduce vulnerable dependencies (CVE-based) in package manifests/lockfiles.
- **`sanitizers.yml`**: Performs heavy blocking runs using Address/Undefined Behavior (ASan+UBSan) and Thread (TSan) sanitizers, generating HTML reports of memory leaks or data races. A push to `main` or a `release/*` branch runs TSan only, because ASan+UBSan already gates every PR in `ci.yml` (a push to `main` also refreshes that PR job's ccache); manual dispatch runs both, and `heavy-checks.yml` runs both nightly.
- **`cache-prune.yml`**: After each run on `main` of a workflow that saves caches (CI, Sanitizers, Heavy Checks, ARM64, CodeQL, Static Analysis, Manual Build, Release; the `check-cache-prune-triggers` pre-commit hook keeps the list complete, #2410), deletes superseded Actions cache entries (all but the newest of each `ccache-<key>` prefix and each OS's FetchContent cache). Manual dispatch defaults to a dry run that lists what it would delete (#1583).
- **`main-health.yml`**: After every run of the main workflows on `main` or a `release/*` branch (CI, which includes the nightly full `clang-tidy`, plus ARM64, CodeQL, Sanitizers, Heavy Checks, OSV and Pre-commit; not the dispatch-only `static-analysis.yml`), opens or updates that branch's tracking issue (`CI: <branch> is red`, labelled `ci-red-main`) while any of them is red, and closes it when all are green again (#1406, #2741). A manual dispatch re-reads `main` and every `release/*` branch.
- **ClusterFuzzLite (`cflite_*.yml`)**: Google's continuous fuzzing suite. Runs on PRs (`cflite_pr.yml`), pushes to main (`cflite_build.yml`), and weekly for batching and pruning corpora (`cflite_batch.yml`, `cflite_prune.yml`).

### Code Quality & Hygiene
- **`pre-commit.yml`**: Runs the `pre-commit` framework (via Python) across all files to enforce syntax hygiene, formatting, and file-level rules configured in `.pre-commit-config.yaml` (pushes and PRs to `main` and `release/*`).
- **`static-analysis.yml`**: Manual full `clang-tidy` run on Linux and on Windows, both blocking (manual dispatch only; `ci.yml`'s nightly schedule runs the full analysis on `main`).
- **`heavy-checks.yml`**: Runs expensive verifications that shouldn't block PR feedback loops, such as generating Coverage reports (nightly, manual dispatch, and pushes to `main` that touch code the benchmarks measure -- `src/Domain`, `src/UI`, `src/Platform`, `ProcessDetailsHistory` -- or the benchmarks, their baseline and tools). The coverage jobs report **line** coverage from `coverage/coverage.lcov` against a floor: the Linux job fails below 95% (#1891), the Windows job warns below 85%; `codecov.yml`'s `auto` target is the per-change ratchet (#1543). A manual dispatch with `scope: benchmark` runs only the benchmark-regression job. The Linux coverage job also runs `tools/check-prereqs.sh` on a fresh image.

### Release & Operations
- **`release.yml`**: Handles compiling production binaries, packaging them (Windows `.zip`; Linux `.tar.gz` and `.deb`; x64 and ARM64), and publishing GitHub Releases (draft first, published last) on `v*.*.*` tags on `main` or on the `release/X.Y` branch the tag names; a `workflow_dispatch` run is a dry run by default (see [Cutting a Release](#cutting-a-release)).
- **`changelog.yml`**: For a strict `vMAJOR.MINOR.PATCH` tag, generates its `CHANGELOG.md` section with `git-cliff` from the tag's own branch history and opens one pull request against the tag's release branch and one against `main`, the canonical file (see [Changelog](#changelog)).
- **`backport.yml`**: On a merged PR carrying a `backport:X.Y` label (or a `/backport` comment on it), cherry-picks it with korthout/backport-action into a `<title> [backport X.Y]` PR against `release/X.Y`, copying the labels and setting the open `vX.Y.*` milestone (#2741; see [Branching Strategy](#branching-strategy)).
- **`pr-labeler.yml`**: Automatically assigns labels (e.g., `bug`, `enhancement`, `docs`) to pull requests based on `.github/labeler.yml` file globs.
- **`copilot-setup-steps.yml`**: Bootstraps the repository environment (CMake, LLVM, etc.) for GitHub Copilot cloud agent sessions.

PR optimization: docs-only pull requests skip compile/test and `clang-tidy` jobs in `ci.yml` to keep feedback fast.

Concurrency: a new push to a PR branch cancels that branch's in-progress runs, but pushes to `main` or a
`release/*` branch never cancel each other. `ci.yml`, `arm64.yml`, `sanitizers.yml` and `heavy-checks.yml` give
each such commit its own concurrency group, so back-to-back merges each get a complete run and a regression is
blamed on the commit that caused it (#1187). `codeql.yml` doesn't cancel `main` runs either, but queues them in one group.
`pre-commit.yml` and `dependency-review.yml` also cancel a PR's superseded run.

Dependabot updates GitHub Actions and Python dependencies weekly.
[OSV Scanner](https://google.github.io/osv-scanner/) scans C++ FetchContent dependencies
(via Syft SBOM generated from `CMakeLists.txt`) and all four Python dependency manifests against the
[OSV vulnerability database](https://osv.dev) on pushes to `main` and `release/*` and the weekly scheduled run. Results appear in the repository's **Security → Code scanning** tab.
Release and CI builds install GLAD's Python dependencies from the hash-locked
`requirements-glad.txt`; the LLVM bootstrap action also verifies the downloaded installer's
SHA-256 digest before executing it.

### Keeping Dependencies Current

Three automated mechanisms check whether a pinned version is stale or vulnerable, and one category is left
manual (#89, #798).

**One-time setup for `renovate.yml`.** Neither step is automatic, since both are repository security settings:

1. Enable Settings → Actions → General → Workflow permissions → "Allow GitHub Actions to create and approve
   pull requests". `gh api repos/mgradwohl/tasksmack/actions/permissions/workflow` must show
   `can_approve_pull_request_reviews: true`, or every run fails.
2. Add a fine-grained PAT for this repository as the `RENOVATE_TOKEN` secret, with Contents, Pull requests,
   Issues (for the Dependency Dashboard issue) and Workflows (the LLVM entry edits `ci.yml`) set to "Read and
   write". `renovate.yml` fails without it rather than falling back to `GITHUB_TOKEN`, whose pull requests
   don't trigger CI.

**Libraries and code we pull in:**

| Source | Freshness (new releases) | Vulnerabilities (CVEs) |
|---|---|---|
| GitHub Actions (`uses: owner/action@sha`) | Dependabot (weekly, Monday) | `dependency-review.yml` (PR-time) |
| Python packages (`requirements.txt`) | Dependabot (weekly, Monday) | `dependency-review.yml` + `osv-scanner.yml` (weekly) |
| C++ `FetchContent` libraries (`cmake/Dependencies.cmake`, `tests/CMakeLists.txt`, `benchmarks/CMakeLists.txt`) | `renovate.yml` (weekly, Wednesday) | `osv-scanner.yml` (weekly) |

Renovate is configured in `.github/renovate.json5` and limited (`enabledManagers`) to its custom regex
manager and its native `pre-commit` manager, so it never manages what Dependabot covers. It matches this
project's `GIT_TAG <sha>  # <tag> - pinned to SHA for supply chain security` convention and always proposes
another full SHA pin. Each dependency has its own `versioningTemplate`, since tag conventions differ
(`release-X.Y.Z` for SDL3, `VER-X-Y-Z` for FreeType, `vX.Y.Zb-docking` for ImGui's docking branch).
`nothings/stb` is pinned to a raw commit with no tags, so it's checked by hand now and then. Renovate only
opens PRs; the same CI gate applies before merge.

**Tools we build with** (CMake, LLVM, Python, Ninja, ccache; see `tools/check-prereqs.sh`), in three tiers:

- *Tier 1, opened automatically:* the pre-commit hook tools (`rev:` pins in `.pre-commit-config.yaml`). The
  `clang-format` mirror's *major* bumps are gated like LLVM's, since its formatting follows the compiler's
  major. Windows CI has no Renovate-tracked Ninja or ccache pins: Ninja comes with the runner image (bump
  `setup-windows-llvm`'s `ninja-version` input when the image changes) and ccache comes from
  `hendrikmuhs/ccache-action`'s pinned release.
- *Tier 2, a PR opens only after someone ticks it on the Dependency Dashboard*
  (`dependencyDashboardApproval: true`), for every update, not just major ones:
  - **LLVM**, grouped into one PR: `LLVM_VERSION`/`LLVM_SEMVER_VERSION` in the workflows that run a compiler
    or clang tool, the `llvm-version` input defaults of `reusable-build-test.yml` (which `ci.yml`'s build jobs
    rely on) and of the `setup-llvm` and `setup-windows-llvm` actions, `LLVM_SUPPORTED_VERSION` in
    `tools/setup-dev.sh`, `$LlvmVersion` in `tools/setup-dev.ps1`, and `TASKSMACK_LLVM_VERSION` in
    `cmake/Options.cmake`. Only stable `llvmorg-X.Y.Z` tags count. Before approving, confirm the Windows
    version is installable from both Chocolatey (CI) and WinGet (`tools/setup-dev.ps1`), which can lag
    upstream (#752). These need updating by hand in the same PR (grep for the old major too): the
    `clang++-22`/`clang-22` names in `CMakePresets.json`'s Linux presets, `release.yml`'s
    `LLVM_LINUX_EXACT_VERSION`, the "LLVM 22" checks in `tools/pgo.sh`/`tools/pgo.ps1`, `tools/common.sh`'s
    clang-format version-discovery list, and the two fallback defaults inside `setup-llvm/action.yml`, kept
    for Copilot's review setup, which can call it with an empty input.
  - **Python interpreter** (major.minor, from stable tags only), grouped: `setup-python-glad/action.yml`,
    `pre-commit.yml`, `$PythonVersion` in `tools/setup-dev.ps1` and `PYTHON_VERSION` in
    `tools/setup-dev.sh`. Update by hand in the same PR: the "3.14" in this guide's setup commands and in
    `requirements.txt`'s header.
  - **CMake, Ninja and ccache** pins in `tools/setup-dev.ps1` (`$CMakeVersion`, `$NinjaVersion`,
    `$CcacheVersion`). Ninja matches the `windows-2025` runner image; CMake is the newest version WinGet
    carries, since its catalog dropped every 3.x, the image's 3.31.x included (#2844). WinGet's feed lags
    upstream, keeps only recent versions, and no CI job installs with this script, so confirm WinGet has
    the version before approving. A pin WinGet no longer carries doesn't stop the script: it installs
    WinGet's newest version with a warning that names the pin to move, so a stale pin shows up on a
    clean machine as that warning, not as a failed setup.
- *Tier 3, manual:* tools that ride the OS, runner or SDK rather than their own releases (`wpr`, `xperf`,
  `wpa`, CPack's NSIS/dpkg/rpmbuild, `gh`, `xvfb`), and optional local-only tools (Inkscape, heaptrack,
  FlameGraph, hotspot).

**`check-prereqs.sh`'s `MIN_*` floors** (`MIN_CMAKE_VERSION`, `MIN_CLANG_VERSION`, `MIN_CCACHE_VERSION`,
`MIN_GIT_VERSION`, `MIN_PYTHON_VERSION`) are minimum requirements for what a contributor has installed, not
versions to fetch. Raise one by hand when the project needs a newer feature.

### Cutting a Release

Releases ship from release branches (#2741; the rules every agent follows are in
[AGENTS.md](AGENTS.md)'s "Branches and backports"). `main` carries all development and is never
frozen for a release. The first release of a minor, `vX.Y.0`, is tagged on the candidate commit the
maintainer approves on `main`, and the maintainer cuts `release/X.Y` from that same commit:

```bash
git push origin <candidate sha>:refs/heads/release/X.Y
```

Later patch releases (`vX.Y.1`, `vX.Y.2`, ...) are tagged on `release/X.Y`'s head once the fixes
approved for them have been backported: every fix merges to `main` first, and a `backport:X.Y`
label on its PR opens the cherry-pick PR against the branch (see [Branching
Strategy](#branching-strategy)). Only release mechanics -- the version bump, release-notes fixes --
go to a release branch without a `main` PR first. A release branch retires once the next minor
ships, unless the maintainer keeps it for security fixes.

Pushing a strict `vMAJOR.MINOR.PATCH` tag on `main` or on the `release/X.Y` branch it names is what
triggers both an official TaskSmack release (`release.yml`: validates the tag, builds Linux/Windows
packages for x64 and ARM64, Authenticode-signs the Windows executables, signs every asset with
Sigstore, and publishes a GitHub Release) and the corresponding `CHANGELOG.md`
update (`changelog.yml`, tag push only -- it has no `workflow_dispatch` trigger). Both run from the
workflow files at the tagged commit, so a release branch needs the current `release.yml` and
`changelog.yml` (backport workflow changes to it like any fix).

`release.yml` can also be dispatched (`workflow_dispatch`). Its `dry_run` input defaults to
**true**: a dry run builds, tests, packages, validates, generates the SBOM and signs exactly like a
tag run (including [Authenticode signing](#windows-code-signing) of the Windows `.zip`s, when
dispatched from `main`, `release/*` or `candidate/*`), but skips `validate`'s tag, branch-ancestry, existing-release and version-equals-tag checks
(it still reads `PROJECT_VERSION`) and creates and publishes nothing -- `create-release` uploads the
would-be release assets as a `release-assets-dry-run` workflow artifact instead. A dry run builds,
tests and validates the ARM64 packages as well as the x64 ones. No tag is pushed, so
`changelog.yml` doesn't run. Rehearse the whole pipeline before tagging, from the branch the tag
will go on, with:

```bash
gh workflow run release.yml --ref release/X.Y -f dry_run=true   # or --ref main for vX.Y.0
```

Dispatching with `dry_run=false` against an existing tag ref re-runs a real release (e.g. to retry a
failed run without creating a new tag); `validate` still checks that the ref really is a tag, so a
stray dispatch from a branch can't impersonate one.

```bash
# 0. For vX.Y.0 only: regenerate the documentation screenshots from the candidate's build with
#    tools/screenshots.sh (Linux) and tools/screenshots.ps1 (Windows), review them for identity, and
#    merge them to main with the backport:X.Y label. The guide and README reference the images by
#    fixed names, so this is an image-only PR (see "Documentation screenshots" under Test).

# 1. Bump CMakeLists.txt's project(TaskSmack VERSION ...) to the target version in its own PR,
#    get it reviewed, and merge it: to main for vX.Y.0 (before the branch is cut), to release/X.Y
#    for a patch release. release.yml's validate job requires this to already match the tag (see
#    Release Environment below), so it must land *before* step 3.

# 2. Check out the branch the tag goes on, fully up to date with that merged bump: main for vX.Y.0
#    (the maintainer-approved candidate commit), release/X.Y for a patch release
git checkout release/X.Y
git pull

# 3. Create and push a strict semver tag matching the version just merged — this is the only trigger
git tag vX.Y.1
git push origin vX.Y.1
```

That single push runs two independent workflows:

- **`release.yml`** validates the tag (see [Release Environment](#release-environment)), builds
  and packages both platforms for both architectures, validates the packaged artifacts (see [Release Artifact
  Validation](#release-artifact-validation)), signs everything with Sigstore and attests its build
  provenance, and publishes the GitHub Release: it creates the release as a **draft**
  with a placeholder body ("Release notes follow." and the two verification commands; not GitHub's
  generated notes, which exceed the release-body limit on a big release, #2801), uploads every asset and `.bundle`, checks the draft's assets match `dist/` by name and
  size, and only then publishes it (`gh release edit --draft=false --latest`), so a failure
  part-way leaves a draft, never a public release with missing assets; re-running reuses that
  draft and overwrites its assets. Then `verify-published-release` downloads the published assets
  and checks them again (see [Release Artifact Validation](#release-artifact-validation)). The
  maintainer's approved release notes then replace the
  placeholder: `gh release edit vX.Y.Z --notes-file <notes.md>`. Keep a pointer to the User
  Guide's [Verifying a Release](docs/guide/user-guide.md#verifying-a-release) in them, as the
  placeholder has.
  See [Release Artifacts](#release-artifacts) for what it contains and [Release
  Reproducibility](#release-reproducibility) for the determinism policy. Watch it under
  **Actions → Release**.
- **`changelog.yml`** generates the tag's `CHANGELOG.md` section from its own branch history and
  opens a `chore/changelog-vX.Y.Z-to-release-X.Y → release/X.Y` pull request and a
  `chore/changelog-vX.Y.Z-to-main → main` one; each must be merged separately -- see
  [Changelog](#changelog) below. Watch it under **Actions → Changelog**.

### Release Artifacts

Each GitHub release (triggered by a `v*.*.*` tag) includes:

- Linux x64 packages: `TaskSmack-<version>-Linux.tar.gz` and `TaskSmack-<version>-Linux.deb`
- Linux aarch64 packages: `TaskSmack-<version>-linux-aarch64.tar.gz` and
  `TaskSmack-<version>-linux-aarch64.deb` (from v0.9.0)
- Windows x64 package: `TaskSmack-<version>-win64.zip`. From v0.9.1 it also carries
  `TaskSmackHelper.exe`, and both executables are Authenticode-signed by Matt Gradwohl (see
  [Windows code signing](#windows-code-signing)); v0.9.0 and earlier are unsigned and have no helper.
- Windows ARM64 package: `TaskSmack-<version>-windows-arm64.zip` (from v0.9.0; plain ARM64, not
  ARM64EC; signed and carrying `TaskSmackHelper.exe` from v0.9.1, like the x64 one)
- Windows build-inputs manifests: `TaskSmack-<version>-win64-build-inputs.json` and
  `TaskSmack-<version>-windows-arm64-build-inputs.json` (from v0.9.1, #2802) -- one per Windows
  `.zip`, written by `.github/actions/windows-build-inputs` after packaging: the MSVC toolset
  version, the SHA-256 of the CRT, STL and UCRT libraries the link consumed (`libcmt.lib`,
  `libvcruntime.lib`, `libcpmt.lib`, `libucrt.lib` and their import-library counterparts), the
  Windows SDK version, the runner image (`ImageOS`/`ImageVersion`), the compiler and the commit. Two
  builds of one commit with equal `inputs` should give the same bytes; see [Release
  Reproducibility](#release-reproducibility) for how a mismatch is read.
- Source SBOM: `tasksmack-<label>-source-sbom.spdx.json` (where `<label>` is the release tag with any non-`[a-zA-Z0-9._-]` characters replaced by `-`) — an SPDX-JSON Software Bill of Materials generated from the **source tree** (not from the built Linux/Windows packages) using [Syft](https://github.com/anchore/syft) via [`anchore/sbom-action`](https://github.com/anchore/sbom-action). It lists components and licenses detected in the repository at the tagged commit to improve supply-chain transparency; it does not inventory the contents of the `.deb`/`.tar.gz`/`.zip` packages themselves.

The x64 names are the ones earlier releases used, so existing download links keep working; only the
ARM64 packages carry an architecture suffix (`CPACK_SYSTEM_NAME`, set in `cmake/Packaging.cmake`
from the compiler's target). Each package's build, test, reproducibility check and validation runs
natively per architecture, as a matrix over `ubuntu-24.04`/`ubuntu-24.04-arm` and
`windows-2025`/`windows-11-arm`, and each build uploads `<os>-packages-<arch>` (for example
`windows-packages-arm64`), and each Windows build also uploads `windows-build-inputs-<arch>`.
`create-release` checks that `dist/` holds exactly these six packages and the two manifests before
signing anything, so a missing or renamed file fails the release.

### Release Artifact Validation

`validate-linux-package` and `validate-windows-package` run after each platform's build job and
before `create-release`, once per architecture, so a failure here blocks signing and publishing.
Each first checks the package is for its architecture: the `.deb`'s `Architecture` field
(`amd64`/`arm64`), and `TaskSmack.exe`'s COFF machine (`0x8664`/`0xAA64`), since an x64 exe would
still run, emulated, on the ARM64 runner. Linux installs the
`.deb` in a bare `ubuntu:24.04` container (no dev tools pre-installed) to exercise the runtime
`Depends` (from `dpkg-shlibdeps` and `cmake/Packaging.cmake`), separately extracts the `.tar.gz`, and launches
each installed/extracted binary with `SDL_VIDEODRIVER=offscreen` (a real Mesa/llvmpipe GL context,
no X server needed). Windows extracts the `.zip` (requiring
`TaskSmack.exe` directly under the single top-level directory, matching the documented layout; a
signed `.zip` must have `TaskSmackHelper.exe` beside it, and an unsigned `.zip` must not carry
one), checks a signed `.zip`'s Authenticode signatures (see [Windows code signing](#windows-code-signing)) and
launches the executable directly on the runner's desktop session, with a temporary copy of Mesa's
llvmpipe beside it (mesa-dist-win on x64, mmozeiko/build-mesa on ARM64; never in the package). Each launch
gets its own `TASKSMACK_CONFIG_DIR` and quits by itself rather than being killed (#2720):
`TASKSMACK_SCREENSHOT=<png>;after=5` writes one frame 5 seconds after startup and closes the window through
the normal close request, so `ShellLayer::onDetach()` saves `config.toml`. The launch must exit 0, log no
`[error]` or `[critical]` line, and leave a `config.toml` with its `[theme]` table, which nothing writes before
the shutdown's save in a run with no input. Each binary is launched twice (#2726): from an empty config dir,
then from a copy of the `config.toml` v0.8.0 saved (`tests/App/fixtures/config-v0.8.0.toml`). The second
launch must save the file in today's format (`config_version`, which v0.8.0 didn't write) and keep the
settings changed in v0.8.0's UI when the fixture was made (theme, font size, refresh rate, history, the
privilege notice, and the User and Threads columns; see `tests/App/fixtures/README.md`).
On Windows the runs must also leave no trace, as a user deleting the portable `.zip`'s folder expects: no new
TaskSmack-named entry under `%APPDATA%` or `%LOCALAPPDATA%` outside the config dirs and `%TEMP%`, and the folder
they ran from deletes with no file still locked. Beyond the clean quit, the Linux checks assert on the actual startup log for confirmation that
assets resolved -- an "Assets directory found:" line, no "Icon font not found" warning, and at
least one "Loaded theme:" line -- since `src/UI/AssetPath.cpp` and `Theme.cpp` both fall back
gracefully on missing assets, so a plain liveness check alone would not catch a broken installed
asset layout. These are still smoke checks, not a functional test suite.

`validate-linux-upgrade` checks the upgrade from v0.8.0 (#2726), x64 only since v0.8.0 published no
arm64 `.deb`. It runs in an `ubuntu:26.04` container, because v0.8.0's `.deb` depends on `libsdl3-0`,
which Ubuntu 24.04 doesn't have. It fetches `TaskSmack-0.8.0-Linux.deb` from the v0.8.0 release and
checks its SHA-256 against the pinned one and the release's asset digest. Then it installs it with apt
and runs v0.8.0 for 5 seconds from the fixture `config.toml`, in `$HOME/.config/tasksmack`, where both
versions read it. It installs this run's `.deb` over it, which must exit 0 with no dpkg file conflict
and upgrade `tasksmack` in place. Every file the new `.deb` ships must be installed as it ships it (docs
and man pages aside, which the image's dpkg doesn't unpack), the helper and its polkit policy checked by name, and no v0.8.0 file the new `.deb` dropped may be left
over. Last, it starts the upgraded `TaskSmack` with no `TASKSMACK_CONFIG_DIR`, so it finds v0.8.0's file
itself. That run gets the same checks as the second smoke launch: a clean quit, no `[error]` line, and
the fixture's settings kept. Windows has no package to upgrade while the `.zip` is portable; an NSIS
upgrade check waits on the installer (#2302).

Both jobs also check what the packages contain, since a bundled dependency's own `install()` rules
can ship development files to users: the `.deb`'s listing (`dpkg -c`) may hold nothing outside
`usr/bin`, `usr/libexec/tasksmack` and `usr/share/{tasksmack,applications,icons,doc,metainfo,pixmaps,man,polkit-1}`, and the `.zip`
may hold no `*.lib`/`*.a` files and no `include/` or `lib/cmake/` directories.

`verify-published-release` runs after `create-release` and checks what users actually download (#2721),
on one x64 Linux and one x64 Windows runner. A tag run downloads the published release (`gh release
download`); a dry run, which publishes nothing, downloads its `release-assets-dry-run` artifact instead,
so every dry run exercises the job, and where the files come from is the only difference. Each leg
checks that the download holds exactly the files `create-release` hashed after signing (every asset and
its `.bundle`, nothing extra), each with the SHA-256 of its `dist/` copy (the job's `sha256sums` output,
not a rebuild). The Linux leg then runs `cosign verify-blob --bundle` on every asset with the User
Guide's certificate identity and issuer (a dry run signs from a branch, so it pins the same workflow at
its own ref), runs `gh attestation verify --signer-workflow mgradwohl/tasksmack/.github/workflows/release.yml`
on a tag run (a dry run makes no attestations), and relaunches the `.tar.gz` with
`validate-linux-package`'s smoke check. The Windows leg checks the `.zip`'s Authenticode signatures
with `validate-windows-package`'s check once signing is on (until #2302 signs it, the step says it's
skipped), and relaunches it with that job's smoke check. Both jobs run the same scripts:
`tools/release-smoke-check.sh`, `tools/release-smoke-check.ps1` and `tools/verify-windows-signatures.ps1`.
On a tag run the release is already public by then, so a failure can't unpublish it: the job fails the
run, its step summary names the check that failed, and the maintainer decides what to do about the
live release.

### Windows code signing

Both Windows `.zip`s' `TaskSmack.exe` and `TaskSmackHelper.exe`, x64 and ARM64, are
Authenticode-signed through [Azure Artifact Signing](https://learn.microsoft.com/azure/artifact-signing/)
(#2302; [code signing policy](docs/code-signing-policy.md)). The signer is the maintainer's validated
identity, `CN=Matt Gradwohl, O=Matt Gradwohl, L=REDMOND, S=wa, C=US`, so UAC and SmartScreen name
**Matt Gradwohl** as the publisher.

| Setting | Value |
|---|---|
| Artifact Signing account | `tasksmacksigning` (endpoint `https://wus2.codesigning.azure.net`) |
| Certificate profile | `tasksmack-release` (Public Trust) |
| Expected signer CN | `Matt Gradwohl` (`WINDOWS_EXPECTED_SIGNER` in `release.yml`, `TASKSMACK_EXPECTED_SIGNER` in CMake) |
| GitHub environment | `release-signing`: admits `v*` tags and the `main`, `release/*` and `candidate/*` branches |
| Sign-in | GitHub OIDC; a tenant-level sign-in (`allow-no-subscriptions`); the environment's `AZURE_CLIENT_ID` and `AZURE_TENANT_ID` secrets name an Entra app whose federated credential trusts `repo:mgradwohl@3945134/tasksmack@1116557716:environment:release-signing`, and which holds the Artifact Signing Certificate Profile Signer role. No client secret exists. |

**When it signs.** `SIGNING_ENABLED` at the top of `release.yml` is true exactly when the run may
enter `release-signing`: every tag run, and dry runs dispatched from `main`, `release/*` or
`candidate/*`. A dry run from any other branch packages both `.zip`s as before #2302: without
`TaskSmackHelper.exe` (the helper never ships unsigned), with `sign-windows` skipped
(`validate-windows-package` and `create-release` accept that skip only then), and with no signature
check. `validate` refuses a real release (a tag run or a `dry_run=false` dispatch) that
`SIGNING_ENABLED` would leave unsigned (#2745). To prove a `release.yml` change end to end from a
PR, push it as a `candidate/<name>` branch and dispatch a dry run from it; that's what the
`candidate/*` rule is for.

With signing on, `release.yml` does this:

1. `build-windows` (both architectures) configures with `-DTASKSMACK_PACKAGE_WINDOWS_HELPER=ON`,
   `-DTASKSMACK_SIGN_AFTER_PACKAGING=ON` and `-DTASKSMACK_EXPECTED_SIGNER=Matt Gradwohl`, packages
   the `.zip` with CPack as usual (the helper unsigned, with a CPack warning saying so) and uploads
   it as the `windows-unsigned-<arch>` artifact, which `create-release` never downloads.
2. `sign-windows` (on `windows-2025`; no checkout, no build; the only job in `release-signing`, and
   the only one with `id-token: write` apart from `create-release`'s Sigstore signing) downloads both
   unsigned `.zip`s, extracts the two executables from each, signs in with `azure/login` and signs all
   four with `azure/artifact-signing-action` (SHA-256 file digest, RFC 3161 SHA-256 timestamp from
   `http://timestamp.acs.microsoft.com`, every credential type but the Azure CLI's excluded). It then
   writes each signed executable back over its own entry in its `.zip`, checks that nothing else in
   the `.zip` changed, and uploads the signed `.zip`s as `windows-packages-<arch>`.
3. `validate-windows-package` (both architectures) runs `cmake/VerifyWindowsSignatures.cmake` on the
   signed `.zip`'s extracted contents: both executables `Valid`, signed by `Matt Gradwohl`,
   timestamped, and by the same subject. Only then can `create-release` hash, Sigstore-sign and
   publish them, so an unsigned or wrongly signed helper can't be published.

Artifact Signing certificates last about three days and rotate, so every check matches the signer's
common name, never a thumbprint or serial number; the timestamp is what keeps a signature valid once
its certificate expires.

To check a run's signatures, download its `windows-packages-<arch>` artifact (or the dry run's
`release-assets-dry-run`), extract the `.zip` and run
`Get-AuthenticodeSignature .\TaskSmack-*\TaskSmack*.exe | Format-List Status, StatusMessage,
SignerCertificate, TimeStamperCertificate`. Each file should read `Valid`, with signer subject
`CN=Matt Gradwohl, ...` and a timestamp certificate.

The NSIS installer #2302 plans isn't built yet: it needs its executables signed before `cpack` runs
(it can't be signed inside afterwards), then its uninstaller and the installer itself.

### Release Environment

`release.yml`'s `validate` job rejects a tag before any build/sign/publish work runs if it is not
strict `vMAJOR.MINOR.PATCH`, does not resolve to a commit on `main` or on the `release/X.Y` branch
the tag names (either's HEAD or an ancestor of it, so a merge landing right after the tag push
doesn't abort the release; #2741), already has a
published (non-draft) GitHub Release (so a re-pushed tag can't silently swap a public release's
assets), or does not match `CMakeLists.txt`'s `project(TaskSmack VERSION ...)` — a missed version
bump fails the release instead of publishing mismatched metadata. Concretely: **bump and merge `CMakeLists.txt`'s
`project(TaskSmack VERSION ...)` to the target version first**, then tag that merged commit on
`main` or `release/X.Y` — see step 1 in [Cutting a Release](#cutting-a-release), which creates the same tag that
also triggers this release workflow. Tagging before the version bump lands fails this check
immediately.

Each platform build job's "Record toolchain versions" step logs the runner image
(`ubuntu-24.04`/`ubuntu-24.04-arm`/`windows-2025`/`windows-11-arm`), full compiler/linker versions (`clang --version`, `ld.lld
--version`), CMake, Ninja, and Python versions to that job's log. Linux additionally pins and
asserts an exact `apt.llvm.org` package version for `clang-22` (`LLVM_LINUX_EXACT_VERSION` in
`release.yml`) rather than accepting whatever the apt repository currently serves for the major
version — see the comment above that variable for the deliberate-upgrade procedure; the arm64
runner installs the same package version from apt.llvm.org's `binary-arm64`. Windows already pins
an exact LLVM semver via `LLVM_SEMVER_VERSION`: Chocolatey's installer on x64, and on ARM64 the
native `woa64` installer pinned by SHA-256 in `.github/actions/setup-windows-llvm`.

### Release Reproducibility

Release builds set `SOURCE_DATE_EPOCH` (from the tagged commit's timestamp) before configuring.
CMake's `string(TIMESTAMP ...)` calls that populate `version.h`'s `BUILD_DATE`/`BUILD_TIME` honor
that variable, so the embedded timestamp reflects the commit being released rather than
wall-clock build time; CPack's `TGZ`/`ZIP` archive generators also honor it for entry timestamps,
so two independent builds of the same tag produce byte-identical archives (before Authenticode
signing; see below). Local/dev builds leave
`SOURCE_DATE_EPOCH` unset and get the real configure-time timestamp, which is fine since dev
builds have no reproducibility requirement.

CPack adds a `.tar.gz`'s entries in the order the filesystem lists the staging directory, which
differs between filesystems (and so between runners), with the build user's owner and umask. On
Linux, `cmake/ReproducibleTarball.cmake` (a CPack post-build script) therefore repacks the `.tar.gz`
with GNU tar: entries sorted by name, owned by root, modes 0755/0644, mtimes set to
`SOURCE_DATE_EPOCH`, and gzip without a name or timestamp. CPackDeb already sorts and normalizes the
`.deb`'s entries.

`version.h` also records whether `git status` saw the checkout clean or dirty at configure time, so
anything a release job leaves untracked in the checkout (ccache's `.ccache/`, for one) must be
gitignored; otherwise the build job's binary says "dirty", a clean rebuild's says "clean", and the
packages differ. A configure with `SOURCE_DATE_EPOCH` set warns, naming the files, when the tree is
dirty.

`release.yml`'s `verify-reproducibility` (Linux) and `verify-reproducibility-windows` jobs each
rebuild their platform's tag from a clean workspace with the same `SOURCE_DATE_EPOCH`, once per
architecture on that architecture's runner, and compare package digests against the corresponding
`build-linux`/`build-windows` leg's artifact -- a missing
matching artifact counts as a failure, not a skip. With signing on, each Windows leg compares against
the **unsigned** `.zip` `build-windows` sent for signing (`windows-unsigned-<arch>`): an Authenticode signature embeds a
timestamp, so signed release `.zip`s are not byte-reproducible by design. The rebuild is configured
without `TASKSMACK_PACKAGE_WINDOWS_HELPER`, so its `.zip` lacks `TaskSmackHelper.exe`; the check then
compares the two `.zip`s file by file and the original's helper against the one the rebuild built. Both are advisory only (`continue-on-error:
true`, not in `create-release`'s `needs`) — a mismatch is surfaced as a workflow warning for
investigation rather than blocking the release, since reproducibility here covers CMake's own
generated files and CPack's archive metadata, not every possible source of binary non-determinism
(e.g. absolute build-path fragments a future dependency might embed in debug info).

On Windows the build is deterministic but its inputs may not be: the runner image's serviced MSVC
CRT and Windows SDK payload can change under an unchanged directory name (v0.9.0's arm64
`TaskSmack.exe` differed between the dry run and the tag run because one image carried VS 18.10's
libraries and the other 18.11's, both as `VC\Tools\MSVC\14.51.36231` and SDK `10.0.26100.0`; #2802).
Each Windows build and rebuild therefore records a build-inputs manifest (see [Release
Artifacts](#release-artifacts)), and `verify-reproducibility-windows`'s "Compare digests" diffs the
two manifests' `inputs` with `.github/actions/windows-build-inputs/compare-build-inputs.ps1`: a
differing `.zip` whose inputs also differ is reported as the runner image's CRT changing, and one
whose inputs match as the build itself not being reproducible. The post-release check does the
same for the dry run's and the published release's manifests of the same commit (both carry them):

```powershell
pwsh .github/actions/windows-build-inputs/compare-build-inputs.ps1 `
    dry-run/TaskSmack-<version>-windows-arm64-build-inputs.json `
    release/TaskSmack-<version>-windows-arm64-build-inputs.json   # exit 0: same inputs; 1: differ
```

Byte-identity across runner images would need the toolset pinned by content (a cached, hash-checked
MSVC and SDK overlay, as `setup-windows-llvm` does for LLVM); the manifest is the record that makes
that a deliberate follow-up rather than a guess.

To verify a published Linux release yourself: clone the tag, export
`SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)`, build with the `release` preset, package with
`cpack -G "TGZ;DEB"`, and compare `sha256sum` of the result against the published asset.

Every asset is signed with Sigstore (keyless OIDC via `cosign sign-blob`) and shipped with a matching `<file>.bundle`. A tag run also records a GitHub artifact attestation (SLSA build provenance, `actions/attest-build-provenance`) covering the same files; a dry run skips it, since attestations are public and permanent, and lists the files a tag run would attest instead (#2673). See the User Guide's [Verifying a Release](docs/guide/user-guide.md#verifying-a-release) for the user-facing `gh attestation verify` and `cosign verify-blob` procedures.

### Changelog

[CHANGELOG.md](CHANGELOG.md) is auto-generated by [git-cliff](https://git-cliff.org/) on every `v*.*.*` tag push via the `changelog` workflow. It follows the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format and groups entries by conventional commit type (`feat` → Added, `fix` → Fixed, etc.).

**Do not edit CHANGELOG.md manually.** The `changelog` workflow prepends the release's section and opens pull requests that must be merged; it never pushes to `main` or a release branch directly.

**Which branch owns the file (#2741):** `main`'s `CHANGELOG.md` is the canonical, complete one. Every release's section is forwarded to it, in the order the releases shipped (a `[0.9.2]` cut after `v0.10.0` sits above `[0.10.0]`). A release branch's copy holds only its own line of releases plus the sections it inherited at the cut. Each section comes from the tag's own branch history, so a fix backported to 0.9.1 is listed under both `[0.9.1]` and `[0.10.0]`.

#### Producing a Changelog

See [Cutting a Release](#cutting-a-release) for the tag-push steps that trigger this -- the same
tag push also triggers `release.yml`. Once pushed, the `changelog` workflow runs automatically
and:

1. Validates the tag is strict semver (`vMAJOR.MINOR.PATCH`). Tags like `v1.0.0-rc1` or `v1.0.0.4` are rejected.
2. Finds the branches that hold the tag's commit: `release/X.Y` when it exists and contains the tag, and `main`, always, as the canonical file's home. A tag on neither aborts the workflow.
3. Finds the previous `vMAJOR.MINOR.PATCH` tag reachable from the tag on its own history (`v0.9.0` for `v0.9.1` on `release/0.9`, even once `main` carries `v0.10.0`).
4. Runs git-cliff (pinned to a fixed version in `changelog.yml`, with `main`'s `cliff.toml`) over that range and prepends the `[X.Y.Z]` section after the header of each target branch's current `CHANGELOG.md`, so every branch's copy of the section is identical.
5. Opens one pull request per target (`chore/changelog-vX.Y.Z-to-release-X.Y → release/X.Y`, `chore/changelog-vX.Y.Z-to-main → main`), labeled `documentation`. If a PR can't be created, the workflow fails with an error that links the compare page for opening it by hand. A rerun reuses the open PR, and a target whose `CHANGELOG.md` already carries the section is skipped.
6. Merge each PR once CI passes. If the release branch is cut after its tag (`release/0.9` was, after `v0.9.0`), label `main`'s changelog PR `backport:X.Y` so the branch gets the section too.

You can watch the progress under **Actions → Changelog** in the GitHub UI.

#### The `CHANGELOG_TOKEN` secret

Pushes and pull requests made with the workflow's own `GITHUB_TOKEN` don't trigger other workflows,
so a changelog or backport PR opened with it never gets the required CI checks. `changelog.yml` and
`backport.yml` therefore push the branch and open the PR with the `CHANGELOG_TOKEN` repository
secret: a **fine-grained personal access token** limited to this repository, with **Contents: Read
and write** and **Pull requests: Read and write** (metadata read is implied). Create it under
*Settings → Developer settings → Fine-grained tokens*, add it under the repository's *Settings →
Secrets and variables → Actions*, and renew it before it expires. If the secret is missing or empty
the workflows fall back to `GITHUB_TOKEN` and emit a warning that CI won't run on the PR.

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
build: bump minimum CMake to 3.30
chore: remove unused include
revert: revert "feat: add network panel"
```

These prefixes map to changelog sections as follows (first match wins; sections appear in this
order, and each entry shows its scope, so `fix(windows): ...` reads **Windows:** ...):

| Commit prefix | Changelog section |
|---|---|
| `security:`, any `(security)` scope, a `fix:`/`build:` that hardens a binary or fixes DLL loading | Security |
| `feat:` | Added |
| `fix:` | Fixed |
| `refactor:`, `style:` | Changed |
| `perf:` | Performance |
| `docs:` | Documentation |
| `ci:`, `build:`, `tools:`; `fix(test\|tools\|ci\|build\|cmake\|coverage\|bench\|fuzz\|perf\|...)`, `feat(tools\|test\|perf)`, `perf(bench\|tools)` | Infrastructure |
| `chore:` | Maintenance |
| `revert:` | Reverted |
| `test:`, `ci(deps):`, `build(deps):` | _(skipped — tests and Dependabot bumps of CI actions and dev tools swamp the release notes)_ |
| `Merge ...`, `Update CHANGELOG.md for ...` | _(skipped — noise)_ |
| Untyped titles | By leading verb: Fix → Fixed, Add/Implement → Added, Speed up/Optimize → Performance, Document → Documentation, Bump → Dependencies, Update → Maintenance, anything else → Changed; build/CI/tooling titles → Infrastructure |

Releases up to 0.8.0 still list conventional commits only, as they did when they were generated.

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
- `windows-coverage-report`
- `clang-tidy-results`, `clang-tidy-results-windows`
- `iwyu-results`
- `benchmark-results`

### CI Artifacts (GitHub CLI)

```bash
gh run download <run-id> -n coverage-report
```

## Branching Strategy

Branch from `main` and open a pull request back to `main`: all development lands there, and it is never frozen for
a release. Releases ship from `release/X.Y` branches (#2741), which the maintainer cuts from the approved candidate
commit; see [Cutting a Release](#cutting-a-release). A fix reaches a release branch by backport, upstream first:

1. Merge the fix to `main` by the normal pull request, with the `backport:X.Y` label (for example `backport:0.9`)
   on it, added before or after the merge.
2. On the merge, `backport.yml` cherry-picks the squashed commit and opens `<original title> [backport X.Y]`
   against `release/X.Y`, copying the labels and putting it on the open `vX.Y.*` milestone. For a label added after
   the merge, or a retry, comment `/backport` on the merged PR.
3. If the cherry-pick conflicts, that backport fails and the action's comment on the original PR gives the manual
   steps: `git cherry-pick -x <squash SHA>` onto a branch from `release/X.Y`, resolve, and open the PR yourself.
4. The backport PR merges like any other: CI green and review threads resolved. Only release mechanics (version
   bumps, release-notes fixes) go to a release branch without a `main` PR first.

CI runs on pull requests to `main` and `release/*` and on pushes to either; the "Main Builds" ruleset requires the
same checks on both. Renovate and Dependabot target `main`; a security bump is backported like any fix.

## Pull Request Process

1. Fork the repository
2. Create a branch from `main`
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
- When evaluating layer boundaries, refer to [tasksmack.md](tasksmack.md).

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
