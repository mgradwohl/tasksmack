# MyProject

A modern C++23 project template with clang toolchain, CMake Presets, Google Test, and VS Code integration.

## Features

- **Modern C++23** with clang as the primary compiler (LLVM 22)
- **CMake 3.28+** build system with CMake Presets and Ninja
- **Auto-generated version header** with version, build type, compiler, and timestamp
- **Precompiled headers** for faster compilation
- **Compiler caching** via ccache/sccache for faster rebuilds
- **Code coverage** with llvm-cov and HTML reports
- **CPack packaging** for distributable archives and installers
- **GNUInstallDirs** for portable installation paths
- **Google Test** for unit testing
- **spdlog** for logging (via FetchContent)
- **clang-tidy** and **clang-format** integration
- **VS Code** tasks, launch configs, and clangd support
- **Cross-platform** support for Linux and Windows

## Getting Started

### Step 1: Create a New Repository from This Template

1. Click the green **"Use this template"** button at the top of this page
2. Select **"Create a new repository"**
3. Name your repository (e.g., `AwesomeApp`)
4. Click **"Create repository"**

### Step 2: Clone Your New Repository

```bash
git clone https://github.com/YOUR_USERNAME/YOUR_REPO_NAME.git
cd YOUR_REPO_NAME
```

### Step 3: Run the Setup Script

The setup script renames all placeholder names (`MyProject`, `MYPROJECT`, `myproject`) throughout the codebase to your project name:

```bash
# Linux/macOS
./setup.sh --name "YourProjectName"

# Windows (PowerShell)
.\setup.ps1 -Name "YourProjectName"

# Optional: include author name
./setup.sh --name "YourProjectName" --author "Your Name"
```

> **Note:** The setup script deletes itself after running - it's only needed once.

### Step 4: Commit the Changes

```bash
git add -A
git commit -m "Initial project setup"
git push
```

### Step 5: Build and Run

Using CMake Presets (recommended):

```bash
# Linux
cmake --preset debug
cmake --build --preset debug
./build/debug/YourProjectName

# Windows (PowerShell)
cmake --preset win-debug
cmake --build --preset win-debug
.\build\win-debug\YourProjectName.exe
```

List all available presets:
```bash
cmake --list-presets
```

### Step 6: Run Tests

```bash
# Linux
ctest --preset debug

# Windows
ctest --preset win-debug
```

## Requirements

### Linux
- Clang 22+ recommended
- CMake 3.28+ (4.2.1+ recommended)
- Ninja
- lld (LLVM linker)
- clang-tidy (static analysis)
- clang-format (code formatting)
- ccache 4.9.1+ (required for faster rebuilds)
- llvm-profdata, llvm-cov (for coverage reports)

```bash
# Ubuntu/Debian (with LLVM APT repository for clang-22)
sudo apt install clang-22 clang-tidy-22 clang-format-22 lld-22 llvm-22 cmake ninja-build ccache
```

### Windows
- LLVM/Clang 22+ (includes clang-tidy, clang-format, lld, llvm-cov)
- Set `LLVM_ROOT` environment variable
- CMake 3.28+
- Ninja
- ccache 4.9.1+ (install via `choco install ccache` or `scoop install ccache`)

#### Windows Development Environment

For Windows builds, you need the `LLVM_ROOT` environment variable set and Visual Studio Developer Shell loaded. Use the provided DevShell script to set up your environment:

```powershell
# Load development environment (sets LLVM_ROOT, loads VS DevShell)
. .\Devshell-Updated.ps1
```

The script:
- Loads Visual Studio Developer Shell
- Sets `LLVM_ROOT` environment variable
- Adds LLVM and CMake to PATH

> **Note:** You may need to update the paths in `Devshell-Updated.ps1` to match your Visual Studio installation.

## Project Structure

```
.
├── CMakeLists.txt          # Main build configuration
├── CMakePresets.json       # CMake presets for all platforms/configs
├── Devshell-Updated.ps1    # Windows dev environment setup
├── setup.sh / setup.ps1    # Project renaming scripts (deleted after use)
├── src/
│   ├── main.cpp            # Application entry point
│   └── version.h.in        # Version header template (generates version.h)
├── tests/
│   ├── CMakeLists.txt      # Test configuration
│   └── test_main.cpp       # Example tests
├── tools/
│   ├── build.sh/ps1        # Build helper scripts
│   ├── configure.sh/ps1    # Configure helper scripts
│   ├── check-prereqs.sh/ps1 # Check prerequisite tools and versions
│   ├── clang-tidy.sh/ps1   # Static analysis
│   ├── clang-format.sh/ps1 # Code formatting
│   ├── check-format.sh/ps1 # Format checking
│   └── coverage.sh/ps1     # Code coverage reports
├── dist/                   # CPack output (generated, gitignored)
│   └── *.zip, *.tar.gz     # Distribution packages
├── coverage/               # Coverage reports (generated, gitignored)
│   └── index.html          # HTML coverage report
├── .clang-format           # Formatting rules
├── .clang-tidy             # Static analysis rules
├── .clangd                 # clangd LSP configuration
├── .github/
│   ├── workflows/ci.yml    # CI/CD pipeline
│   ├── ISSUE_TEMPLATE/     # Bug report and feature request templates
│   ├── pull_request_template.md  # PR checklist
│   └── dependabot.yml      # Automated dependency updates
├── SECURITY.md             # Security policy and vulnerability reporting
└── .vscode/
    ├── tasks.json          # Build tasks (platform-aware)
    ├── launch.json         # Debug configurations (platform-aware)
    └── settings.json       # Editor settings
```

## VS Code Integration

Install recommended extensions (VS Code will prompt you):
- **clangd** (LLVM) - IntelliSense and code completion
- **CodeLLDB** - Debugging support
- **CMake Tools** - CMake integration

### Build Tasks

Tasks automatically use the correct preset for your platform (Linux or Windows):

- `Ctrl+Shift+B` - Build debug (default)
- Use Command Palette (`Ctrl+Shift+P`) → "Tasks: Run Task" for other configurations

Available tasks:
- CMake: Configure/Build Debug, RelWithDebInfo, Release, Optimized
- CMake: Build ASan+UBSan (Linux), Build TSan (Linux)
- CMake: Test Debug
- Clang-Tidy, Clang-Format, Check Format
- Check Prerequisites

### Debugging

Launch configurations automatically use the correct paths for your platform:

- `F5` - Debug with current launch configuration
- Configurations: Debug (Debug), Debug (RelWithDebInfo), Run (Release), Run (Optimized)

## Build Types

Available as CMake presets (use `cmake --list-presets` to see all):

| Preset (Linux) | Preset (Windows) | Description |
|----------------|------------------|-------------|
| `debug` | `win-debug` | Debug symbols, no optimization, security hardening |
| `relwithdebinfo` | `win-relwithdebinfo` | Debug symbols + optimization |
| `release` | `win-release` | Optimized, no debug symbols |
| `optimized` | `win-optimized` | LTO, march=x86-64-v3, stripped |
| `coverage` | `win-coverage` | Debug + code coverage instrumentation |
| `asan-ubsan` | — | AddressSanitizer + UBSan (Linux only) |
| `tsan` | — | ThreadSanitizer (Linux only) |

## Code Quality Tools

### Formatting
```bash
./tools/clang-format.sh        # Apply formatting (modifies files)
./tools/check-format.sh        # Check compliance (no changes)
```

### Static Analysis
```bash
./tools/clang-tidy.sh debug    # Run clang-tidy
```

> **Note:** The build uses precompiled headers (PCH) for faster compilation. Since PCH files are compiler-version-specific, the clang-tidy target automatically strips PCH flags from the compile commands. This allows clang-tidy to work even if its version differs from the compiler version.

### Code Coverage

Generate code coverage reports using llvm-cov. Reports are output to the `coverage/` folder (gitignored).

```bash
# Linux - generates HTML report
./tools/coverage.sh

# Linux - generate and open in browser
./tools/coverage.sh --open

# Windows
.\tools\coverage.ps1
.\tools\coverage.ps1 -OpenReport
```

The coverage report shows:
- Line-by-line coverage highlighting (green = covered, red = not covered)
- Coverage percentages per file and overall
- Branch coverage for conditional statements

View the report at `coverage/index.html` after generation.

### Sanitizers (Linux only)

Sanitizers catch bugs that unit tests and static analysis miss. Two presets are available:

**AddressSanitizer + UndefinedBehaviorSanitizer** — catches memory errors and undefined behavior:
```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
# Or run directly:
ASAN_OPTIONS=detect_leaks=1 ./build/asan-ubsan/MyProject
```

**ThreadSanitizer** — catches data races in multithreaded code (cannot combine with ASan):
```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

Common issues detected:
| Sanitizer | Detects |
|-----------|---------|
| ASan | Buffer overflows, use-after-free, double-free, memory leaks |
| UBSan | Signed overflow, null pointer dereference, misaligned access |
| TSan | Data races, deadlocks, thread leaks |

> **Note:** Sanitizers add runtime overhead (~2-5x slower). Use them during development and CI, not in production builds.

### Compiler Warnings

The project applies a comprehensive set of compiler warnings tuned for Clang on both Windows and Linux. Warnings are applied per-target (not globally) to avoid affecting third-party dependencies.

**Baseline warnings (all platforms):**
```
-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough
-Wnull-dereference -Wdouble-promotion -Wcast-align -Wundef -Werror=return-type
```

**Linux/macOS additions:**
```
-Wmisleading-indentation
```

**Windows suppressions** (reduces noise from MSVC STL/SDK headers):
```
-Wno-unknown-pragmas -Wno-nonportable-system-include-path
```

**CMake options:**
| Option | Default | Description |
|--------|---------|-------------|
| `MYPROJECT_ENABLE_WARNINGS` | `ON` | Enable all warnings |
| `MYPROJECT_WARNINGS_AS_ERRORS` | `ON` | Treat warnings as errors (`-Werror`) |

To disable warnings-as-errors for local development:
```bash
cmake --preset debug -DMYPROJECT_WARNINGS_AS_ERRORS=OFF
```

## Packaging with CPack

Create distributable packages using CPack. Packages are output to the `dist/` folder (gitignored).

```bash
# First, build the release configuration
cmake --preset release          # Linux
cmake --build --preset release

# Create a ZIP package
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

Packages are created in `dist/` with the naming format: `ProjectName-Version-Platform.ext`

## Version Header

The project auto-generates a `version.h` header at configure time with:
- Project version (from CMakeLists.txt)
- Build type (Debug, Release, etc.)
- Compiler ID and version
- Build timestamp (`__DATE__` and `__TIME__`)

Usage:
```cpp
#include "version.h"

spdlog::info("{} v{}", myproject::Version::PROJECT_NAME, myproject::Version::STRING);
spdlog::debug("Built: {} {}", myproject::Version::BUILD_DATE, myproject::Version::BUILD_TIME);
```

The header is generated to `build/<preset>/generated/version.h` and provides both C macros (`MYPROJECT_VERSION`) and a C++ namespace (`myproject::Version`).

## Adding Dependencies

Use CMake's `FetchContent` for header-only or source-based dependencies. Always use `SYSTEM` to suppress compiler warnings from third-party code:

```cmake
FetchContent_Declare(
    mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM  # Treat as system headers to suppress warnings
)
FetchContent_MakeAvailable(mylib)
```

Then link to your target:

```cmake
target_link_libraries(YourProjectName PRIVATE mylib)
```

### Optional: Shared FetchContent cache

By default, dependencies fetched via FetchContent live under each build dir’s `_deps`. To speed up repeated config/builds across presets while keeping the source tree clean, enable the shared cache (it defaults to `.cache/fetchcontent` beside the source root and is gitignored/excluded from tooling):

```bash
cmake --preset debug -DMYPROJECT_ENABLE_FETCHCONTENT_CACHE=ON

cmake --preset win-debug -DMYPROJECT_ENABLE_FETCHCONTENT_CACHE=ON
```

To pick a custom cache directory (also reused by CPM if you add it), set either `MYPROJECT_FETCHCONTENT_CACHE_DIR` or the standard `FETCHCONTENT_BASE_DIR`:

```bash
cmake --preset debug -DMYPROJECT_ENABLE_FETCHCONTENT_CACHE=ON -DMYPROJECT_FETCHCONTENT_CACHE_DIR=/path/to/cache
```

## CI/CD

This template includes a GitHub Actions workflow (`.github/workflows/ci.yml`) that automatically:
- Builds on Linux (Ubuntu 24.04) and Windows
- Runs all tests
- Runs sanitizers (ASan+UBSan, TSan) on Linux
- Checks code formatting (clang-format)
- Runs static analysis (clang-tidy)
- Generates code coverage reports

**Dependabot** is configured to automatically create PRs for GitHub Actions updates (weekly).

## Design Decisions

This section documents intentional design choices to help template users understand why certain approaches were taken.

### What This Template Uses

| Choice | Rationale |
|--------|----------|
| **CMake Presets** | Single source of truth for all build configurations. No separate toolchain files needed. |
| **FetchContent** | Simple dependency management built into CMake. No external package manager required. |
| **Platform default C++ libraries** | libstdc++ on Linux, MSVC STL on Windows. More portable than forcing libc++ everywhere. |
| **clangd for IDE** | Superior C++ language server. cpptools disabled to avoid conflicts. |
| **Individual CMake options** | Granular control (e.g., `MYPROJECT_ENABLE_PCH`) rather than umbrella "Developer Mode". |

### Disabled Clang-Tidy Checks

Some checks are disabled in `.clang-tidy` due to false positives or excessive noise:

| Check | Reason |
|-------|--------|
| `bugprone-exception-escape` | Traces through spdlog/STL exception paths, creates wall of noise |
| `misc-include-cleaner` | False positives with Windows headers (`<windows.h>`, `<cstdio>`) |
| `bugprone-easily-swappable-parameters` | Too opinionated for general use |
| `modernize-use-trailing-return-type` | Style preference, not a correctness issue |
| `readability-identifier-naming` | Conflicts with different naming conventions |
| `readability-identifier-length` | Too restrictive for template code |
| `readability-magic-numbers` | Too noisy for a template project |
| `readability-implicit-bool-conversion` | Common C++ idiom |
| `portability-avoid-pragma-once` | `#pragma once` is the project standard |

### Why Certain Features Are Not Included

| Feature | Reason Not Included |
|---------|---------------------|
| **CMake toolchain files** | Presets provide complete build configurations already |
| **install/export config** | This is an app template, not a library with downstream consumers |
| **libc++** | Adds complexity; platform defaults work well and are more portable |
| **CPM.cmake** | FetchContent is sufficient; CPM adds another dependency to manage |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

- Use the **issue templates** for bug reports and feature requests
- PRs will be checked against the **PR template** checklist
- Security issues should be reported per [SECURITY.md](SECURITY.md)

## License

MIT License - See [LICENSE](LICENSE) file.
