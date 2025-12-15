# TaskSmack - Copilot Instructions

This file provides guidance for GitHub Copilot coding agents working on this project.

## Project Overview

TaskSmack is a cross-platform system monitor and task manager built with modern C++23 and OpenGL. The project emphasizes:
- Modern C++23 with clang as the primary toolchain (LLVM 22+)
- Dear ImGui (docking) + ImPlot for immediate-mode UI
- OpenGL 3.3+ rendering with GLFW for windowing
- Clean architecture with strict layer separation (Platform → Domain → UI)
- Cross-platform support (Linux, Windows)

## Development Environment

### Required Tools
- **Compiler:** Clang 22+ with lld linker
- **Build System:** CMake 3.28+ with Ninja and CMake Presets
- **Compiler Cache:** ccache 4.9.1+ (required for faster rebuilds)
- **Static Analysis:** clang-tidy (included with LLVM)
- **Code Formatting:** clang-format (included with LLVM)
- **Coverage:** llvm-profdata, llvm-cov (included with LLVM)
- **Testing:** Google Test (via CMake FetchContent)
- **Logging:** spdlog (via CMake FetchContent)

### Build-Time Dependencies
- **Python 3** with **python3-jinja2** - Required by GLAD v2.0.8 to generate OpenGL loader code
  ```bash
  # Ubuntu/Debian
  sudo apt install python3 python3-jinja2
  ```

### Graphics and UI Stack
- **OpenGL:** 3.3+ core profile for rendering
- **GLFW:** 3.4 for windowing and input (via FetchContent)
- **GLAD:** v2.0.8 for OpenGL function loading (via FetchContent, requires Python + Jinja2)
- **Dear ImGui:** docking branch for immediate-mode UI (via FetchContent)
- **ImPlot:** master branch for plotting and charts (via FetchContent)

### Build Commands

Configure and build (Linux):
```bash
cmake --preset debug
cmake --build --preset debug
```

Configure and build (Windows):
```powershell
cmake --preset win-debug
cmake --build --preset win-debug
```

Run tests:
```bash
ctest --preset debug        # Linux
ctest --preset win-debug    # Windows
```

List available presets:
```bash
cmake --list-presets
```

### VS Code Tasks
The project includes predefined tasks in `.vscode/tasks.json` that auto-detect platform:
- **CMake: Build Debug** - Build with debug symbols (default, Ctrl+Shift+B)
- **CMake: Build RelWithDebInfo** - Build with optimizations + debug info
- **CMake: Build Release** - Build optimized release version
- **CMake: Build Optimized** - Build with LTO, march=x86-64-v3, stripped
- **CMake: Build ASan+UBSan (Linux)** - Build with AddressSanitizer + UBSan
- **CMake: Build TSan (Linux)** - Build with ThreadSanitizer
- **CMake: Test Debug** - Execute test suite
- **Clang-Tidy** - Run static analysis
- **Clang-Format** - Format all source files
- **Check Format** - Check formatting compliance
- **CPack: Create Package** - Create distributable ZIP package
- **Coverage: Generate Report** - Generate HTML code coverage report
- **Check Prerequisites** - Verify all required tools are installed with correct versions

## Coding Standards

### Language and Style
- **C++ Standard:** C++23 (required)
- **Compiler:** Primarily validated with clang
- **Formatting:** Use `.clang-format` (LLVM base, Allman braces)
  - Run `./tools/clang-format.sh` before commits
- **Static Analysis:** Use `.clang-tidy` configuration
  - Run `./tools/clang-tidy.sh` regularly
  - Note: PCH flags are automatically stripped from compile_commands.json for clang-tidy compatibility

### Compiler Warnings
The project applies comprehensive warnings tuned for Clang on Windows and Linux:

- **Baseline (all platforms):** `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference -Wdouble-promotion -Wcast-align -Wundef -Werror=return-type`
- **Linux/macOS:** Adds `-Wmisleading-indentation`
- **Windows:** Adds `-Wno-unknown-pragmas -Wno-nonportable-system-include-path` (suppresses MSVC STL/SDK noise)
- **Warnings-as-errors:** Controlled by `TASKSMACK_WARNINGS_AS_ERRORS` (default ON)

Warnings are applied per-target via `tasksmack_apply_default_warnings()` — they do NOT affect third-party dependencies.

### IDE Configuration (clangd)
The `.clangd` file configures clangd for IDE integration:
- Adds project-specific flags and system header prefixes
- **Removes PCH flags** (`-Xclang`, `-include-pch`, `-emit-pch`, etc.) that clangd cannot handle
- This does NOT disable PCH in builds - only tells clangd to ignore those flags

### Include Order

1. Matching header (`.cpp` files only)
2. Project headers (`src/`, `include/`, `tests/`), alphabetical
3. Third-party libraries (spdlog, gtest, imgui, implot), alphabetical
4. C++ standard library headers, alphabetical
5. Other (fallback)

Separate each group with a blank line. Use `#pragma once` in all headers.

**Example:**
```cpp
#include "MyClass.h"

#include "src/utils/Helper.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
```

### OpenGL/GLFW Header Ordering

When using GLFW with GLAD, **always define `GLFW_INCLUDE_NONE` before including GLFW headers** to prevent GL header conflicts. Protect these defines with `clang-format off/on`:

```cpp
// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// clang-format on

#include <glad/gl.h>
```

**Why:** GLFW normally tries to include OpenGL headers automatically. Since we use GLAD for function loading, we must prevent GLFW from including conflicting GL headers.

### Naming Conventions
- **Classes/Types:** PascalCase (e.g., `DataProcessor`, `FileHandler`)
- **Functions/Methods:** camelCase (e.g., `processData`, `handleFile`)
- **Member Variables:** camelCase with `m_` prefix for private members (e.g., `m_data`)
- **Constants:** UPPER_SNAKE_CASE (e.g., `MAX_BUFFER_SIZE`)

### Preprocessor Directives
- Use `#ifdef X` instead of `#if defined(X)` for simple checks
- Use `#ifndef X` instead of `#if !defined(X)`
- Compound conditions like `#if defined(X) && !defined(Y)` stay as-is

## Project Structure

```
Devshell-Updated.ps1  # Windows dev environment setup (in project root)

src/
├── main.cpp          # Application entry point
├── version.h.in      # Version header template (CMake generates version.h)
├── Core/             # Application loop, windowing, layer management
│   ├── Application.h/cpp
│   ├── Window.h/cpp
│   └── Layer.h
├── UI/               # ImGui integration and rendering
│   └── UILayer.h/cpp
├── App/              # Application-specific UI layers and panels
│   ├── ShellLayer.h/cpp
│   └── Panels/
│       ├── ProcessesPanel.h/cpp
│       ├── ProcessDetailsPanel.h/cpp
│       └── SystemMetricsPanel.h/cpp
├── Domain/           # Cross-platform business logic and data models
│   ├── ProcessModel.h/cpp
│   ├── SystemModel.h/cpp
│   ├── BackgroundSampler.h/cpp
│   ├── ProcessSnapshot.h
│   └── History.h
└── Platform/         # OS-specific system probes and interfaces
    ├── ProcessTypes.h
    ├── SystemTypes.h
    ├── IProcessProbe.h
    ├── ISystemProbe.h
    ├── IProcessActions.h
    ├── Factory.h
    └── Linux/
        ├── LinuxProcessProbe.h/cpp
        ├── LinuxProcessActions.h/cpp
        ├── LinuxSystemProbe.h/cpp
        └── Factory.cpp

tests/
├── CMakeLists.txt    # Test configuration
├── test_main.cpp     # Test entry point
├── Domain/           # Domain layer tests
│   ├── test_ProcessModel.cpp
│   ├── test_SystemModel.cpp
│   ├── test_History.cpp
│   └── test_BackgroundSampler.cpp
└── Mocks/            # Mock implementations for testing
    └── MockProbes.h

tools/
├── build.sh/ps1        # Build helper scripts
├── configure.sh/ps1    # Configure helper scripts
├── check-prereqs.sh/ps1  # Check prerequisite tools and versions
├── clang-tidy.sh/ps1   # Static analysis
├── clang-format.sh/ps1 # Code formatting
├── check-format.sh/ps1 # Format checking
└── coverage.sh/ps1     # Code coverage reports

.github/
├── workflows/ci.yml    # CI/CD pipeline
├── ISSUE_TEMPLATE/     # Bug report and feature request templates
├── pull_request_template.md  # PR checklist
└── dependabot.yml      # Automated dependency updates

dist/                   # CPack output (gitignored)
coverage/               # Coverage reports (gitignored)
```

### Architecture Layers

TaskSmack follows a strict layered architecture with clear dependency direction:

```
App (ShellLayer, Panels)
    ↓
UI (ImGui integration)
    ↓
Core (Application loop, Window, GLFW)
    ↓
Domain (ProcessModel, SystemModel, History)
    ↓
Platform (OS-specific probes)
    ↓
OS APIs (Linux: /proc/*, Windows: NtQuery*)
```

**Rules:**
- **Platform layer** provides raw counters from OS APIs via probe interfaces
- **Domain layer** transforms counters into immutable snapshots and maintains history
- **UI layer** consumes snapshots and renders with ImGui/ImPlot
- **OpenGL calls** are confined to UI and Core; Domain and Platform are graphics-agnostic
- Domain and Platform never depend on UI, Core, or graphics libraries
## Engineering Workflow
- When making changes to the project structure, scripts (.sh or .ps1), clang files (.clang-format, .clang-tidy, .clangd), or CMake files, update README.md accordingly and copilot-instructions.md if needed and ensure setup scripts and workflow.yml files reflect changes.
- If new folders are created under the project root, make sure to make a smart choice to .gitignore them if needed and exclude them from clangd, clang-format and clang-tidy configurations.
- When adding a new component or dependency ensure to use CMake FetchContent where possible and document the addition in README.md, and update setup scripts, project scripts, readmes, workflow.yml files as needed, and copilot-instructions.md if needed. Also update clangd, clang-format and clang-tidy configurations to exclude the new dependency if needed. 
- If FetchContent downloads need to be reused across presets, enable the shared cache via `TASKSMACK_ENABLE_FETCHCONTENT_CACHE=ON` (optional). The default cache location is `.cache/fetchcontent` beside the source root (gitignored). Respect a user-specified `TASKSMACK_FETCHCONTENT_CACHE_DIR` or the standard `FETCHCONTENT_BASE_DIR` environment variable when provided.
- **GLAD dependency:** GLAD v2.0.8 requires Python 3 with python3-jinja2 at build time to generate OpenGL loader code. Ensure this dependency is documented when adding or modifying GLAD-related code.

## Platform/Domain Design Pattern

TaskSmack separates OS-specific data collection from cross-platform calculations using a **Probe → Domain** pattern:

### Probe Layer (Platform/)
- **Returns raw counters** from OS APIs (e.g., cumulative CPU ticks, memory bytes)
- **Stateless readers** - no computation, no caching
- **Capability reporting** - declares what metrics the OS provides
- **Example:** `LinuxProcessProbe::enumerate()` returns `ProcessCounters` with `userTime`, `systemTime` in raw ticks

### Domain Layer (Domain/)
- **Computes deltas and rates** from counter differences over time
- **Maintains previous state** to calculate changes (e.g., CPU% from tick deltas)
- **Publishes immutable snapshots** for UI consumption
- **Example:** `ProcessModel::refresh()` computes CPU% from current vs. previous `ProcessCounters`

### Why This Separation?
- **Testability:** Domain calculations can be unit tested with mock probes
- **No `#ifdef` soup:** Platform-specific code isolated to probe implementations
- **Consistent semantics:** CPU%, rates, and percentages calculated the same way on all platforms
- **Clear ownership:** Probes own I/O, Domain owns math, UI owns presentation

### Example Flow
```
1. Platform probe reads /proc/[pid]/stat -> ProcessCounters(userTime=1000, systemTime=500)
2. Domain model compares with previous -> delta_user=100, delta_system=50 over 1 second
3. Domain model computes -> cpuPercent = (delta_user + delta_system) / totalCpuDelta * 100
4. Domain model publishes -> ProcessSnapshot(cpuPercent=12.5) 
5. UI renders -> "CPU: 12.5%"
```

## UI and ImGui Patterns

### Panel Organization
- **Panels inherit from `Panel` base class** with `onAttach()`, `onDetach()`, `onUpdate(deltaTime)`, `render(bool* open)`
- **Panels own their data models** (e.g., `ProcessesPanel` owns `ProcessModel` and `BackgroundSampler`)
- **ShellLayer manages panel visibility** via bool flags (`m_ShowProcesses`, `m_ShowMetrics`, etc.)
- **Docking is set up in ShellLayer** via `setupDockspace()`

### ImGui Window Lifecycle
```cpp
class MyPanel : public Panel
{
public:
    void onAttach() override
    {
        // Initialize data models, start background samplers
        m_Model = std::make_unique<Domain::MyModel>(...);
        m_Sampler = std::make_unique<Domain::BackgroundSampler>(...);
    }
    
    void onDetach() override
    {
        // Stop samplers, cleanup resources
        m_Sampler.reset();  // Stops background thread
        m_Model.reset();
    }
    
    void render(bool* open) override
    {
        if (ImGui::Begin("My Panel", open))
        {
            // Render panel contents
            auto snapshots = m_Model->snapshots();  // Lock-free read
            // ... render snapshots
        }
        ImGui::End();
    }
};
```

### Background Sampling Pattern
- **Use `BackgroundSampler` for non-blocking updates** - UI thread reads latest snapshot atomically
- **Never call probe methods from UI thread** - always go through domain models
- **Sampler owns refresh interval** (e.g., 1 second) and runs on background thread with `std::jthread`

### ImGui Tables
- Use `ImGui::BeginTable()` / `ImGui::EndTable()` for structured data
- Enable sorting with `ImGuiTableFlags_Sortable`
- Use `ImGui::TableSetupColumn()` to define columns with appropriate flags
- Call `ImGui::TableHeadersRow()` after column setup

## Testing

### Test Framework
- Use **Google Test (gtest)** for all tests
- Tests live in `tests/` directory with subdirectories matching source structure
- Run via `ctest --preset debug`

### Test Organization
- **Tests are organized by layer:** `tests/Domain/`, `tests/Platform/`, `tests/Integration/`
- **Test files use pattern:** `test_ClassName.cpp`
- **Namespace structure:** Wrap tests in the component's namespace (e.g., `Domain`) and an anonymous namespace for test-local types

**Example:**
```cpp
namespace Domain
{
namespace
{

// Test fixtures and helpers here

TEST(ProcessModelTest, BasicFunctionality)
{
    // Test implementation
}

} // namespace
} // namespace Domain
```

### Test Patterns

**Section Separators:** Use descriptive section comments to organize related tests:
```cpp
// ========== Basic Operations ==========
TEST(MyTest, Create) { /* ... */ }
TEST(MyTest, Update) { /* ... */ }

// ========== Edge Cases ==========
TEST(MyTest, EmptyInput) { /* ... */ }
```

**Mock Classes:** Define mock implementations outside the anonymous namespace to work with `std::make_unique`:
```cpp
namespace Domain
{

// Mock class visible to entire file
class MockProcessProbe : public Platform::IProcessProbe
{
    // Mock implementation
};

namespace
{
    // Tests that use MockProcessProbe
} // namespace
} // namespace Domain
```

**Floating-Point Comparisons:** Use `EXPECT_DOUBLE_EQ` for floating-point comparisons, not `EXPECT_EQ`:
```cpp
EXPECT_DOUBLE_EQ(result.cpuPercent, 12.5);
```

### Code Coverage
- Run `./tools/coverage.sh` to generate HTML coverage report
- Report generated at `coverage/index.html`
- Uses llvm-cov with Clang instrumentation

### Example Test Structure
```cpp
#include <gtest/gtest.h>

#include "src/Domain/ProcessModel.h"
#include "tests/Mocks/MockProbes.h"

namespace Domain
{

class MockProcessProbe : public Platform::IProcessProbe
{
public:
    // Mock implementation
};

namespace
{

// ========== Basic Functionality ==========

TEST(ProcessModelTest, InitialState)
{
    auto probe = std::make_unique<MockProcessProbe>();
    ProcessModel model(std::move(probe));
    EXPECT_EQ(model.processCount(), 0);
}

TEST(ProcessModelTest, RefreshUpdatesSnapshots)
{
    auto probe = std::make_unique<MockProcessProbe>();
    ProcessModel model(std::move(probe));
    model.refresh();
    EXPECT_GT(model.processCount(), 0);
}

// ========== CPU Calculation ==========

TEST(ProcessModelTest, CalculatesCpuPercent)
{
    auto probe = std::make_unique<MockProcessProbe>();
    ProcessModel model(std::move(probe));
    model.refresh();
    const auto& snapshots = model.snapshots();
    if (!snapshots.empty())
    {
        EXPECT_DOUBLE_EQ(snapshots[0].cpuPercent, 0.0);
    }
}

} // namespace
} // namespace Domain
```

## Common Tasks

### Adding a New Feature
1. Create/modify source files in appropriate layer (`src/Platform/`, `src/Domain/`, `src/App/`)
2. Update `TASKSMACK_SOURCES` in CMakeLists.txt if adding files
3. Add tests in corresponding `tests/` subdirectory
4. Run clang-format
5. Run clang-tidy
6. Build and test

### Adding a New Platform Probe
1. Define raw counter struct in `Platform/XxxTypes.h`
2. Define probe interface in `Platform/IXxxProbe.h`
3. Implement platform-specific probe in `Platform/Linux/LinuxXxxProbe.h/cpp`
4. Update factory in `Platform/Linux/Factory.cpp`
5. Create domain model in `Domain/XxxModel.h/cpp` to compute deltas/rates
6. Add tests in `tests/Domain/test_XxxModel.cpp` with mocks
7. Update CMakeLists.txt with new source files

**Key principles:**
- Probes return **raw counters**, not computed values (e.g., cumulative CPU time, not CPU%)
- Domain layer computes **deltas and rates** from counter differences
- Domain models maintain **previous state** for delta calculations
- UI consumes **immutable snapshots** from domain models

### Adding Dependencies
- Use CMake `FetchContent` for header-only libraries (see spdlog, imgui, implot examples in CMakeLists.txt)
- **Always use `SYSTEM` keyword** in `FetchContent_Declare` to suppress warnings from third-party code
- Update CMakeLists.txt and document in README.md
- For GLAD-like dependencies that require build-time tools (Python, Jinja2, etc.), document those requirements

**Example:**
```cmake
FetchContent_Declare(
    mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM  # Treat as system headers to suppress warnings
)
FetchContent_MakeAvailable(mylib)
```

**Special case - GLAD:**
GLAD v2.0.8 uses a CMake script that requires Python 3 and Jinja2 at build time:
```cmake
# GLAD requires Python 3 with jinja2 at build time
FetchContent_MakeAvailable(glad)
add_subdirectory("${glad_SOURCE_DIR}/cmake" glad_cmake)
glad_add_library(glad_gl_core_33 REPRODUCIBLE EXCLUDE_FROM_ALL LOADER API gl:core=3.3)
```

## Best Practices

### Code Quality
- **Minimal changes:** Make the smallest possible changes to achieve the goal
- **DRY principle:** Don't repeat yourself; extract common code
- **RAII:** Use smart pointers and RAII for resource management
- **Const correctness:** Mark methods and variables `const` when appropriate
- **Error handling:** Check return values, use exceptions sparingly

### Modern C++23 Features
- **Prefer modern alternatives:** Use `std::string::contains()`, `std::string::starts_with()` over `find()` or `compare()`
- **Use ranges and views:** Prefer `std::views::reverse` for reverse iteration and `std::ranges` algorithms over raw loops
- **Parameter passing:** Pass configuration structs by value and use `std::move` in constructors
- **String formatting:** Use `std::format` or `std::print` for type-safe formatting

**Example:**
```cpp
// Good - C++23
if (path.starts_with("/proc/"))
{
    auto lines = file | std::views::reverse | std::views::take(10);
}

// Avoid - pre-C++23
if (path.compare(0, 6, "/proc/") == 0)
{
    // Manual reverse iteration
}
```

### Performance
- Avoid unnecessary copies (use const references, move semantics)
- Profile before optimizing

### Documentation
- Use clear, descriptive names that minimize need for comments
- Add comments only when code intent is not obvious
- Keep README.md up to date

### Git Workflow
- Write clear commit messages
- Keep commits focused and atomic
- Ensure code builds and tests pass before committing
- Post commit monitor the CI results for issues and address any problems promptly

## Common Pitfalls to Avoid

- Don't use `using namespace std` in headers
- Don't ignore clang-tidy warnings
- Don't skip running tests before committing
- Don't add unnecessary dependencies
- Don't commit without running clang-format
- Don't forget `GLFW_INCLUDE_NONE` before including GLFW headers when using GLAD
- Don't put computed values (CPU%, rates) in Platform probes - they belong in Domain models
- Don't call Platform probes directly from UI - use Domain models as the interface
- Don't use `EXPECT_EQ` for floating-point comparisons - use `EXPECT_DOUBLE_EQ`
- Don't define mock classes inside anonymous namespaces if using `std::make_unique`

## Design Decisions

This section documents intentional design choices and rejected alternatives.

### What We Use
- **CMake Presets** as the single source of truth for build configurations (not toolchain files)
- **FetchContent** for dependencies (not CPM.cmake - adds complexity without benefit for this template)
- **Platform default C++ standard libraries** (libstdc++ on Linux, MSVC STL on Windows - not libc++)
- **Individual CMake options** for features (not an umbrella "Developer Mode" switch)
- **clangd** for IDE integration (cpptools disabled to avoid conflicts)

### Disabled Clang-Tidy Checks
These checks are disabled in `.clang-tidy` due to false positives or excessive noise:

| Check | Reason |
|-------|--------|
| `bugprone-exception-escape` | Traces through spdlog/STL exception paths, creates wall of noise |
| `bugprone-easily-swappable-parameters` | Too opinionated; common in UI callbacks |
| `misc-include-cleaner` | Doesn't work correctly with Windows headers (`<windows.h>`, `<cstdio>`) |
| `misc-const-correctness` | Too noisy; many false positives with locks and iterators |
| `misc-non-private-member-variables-in-classes` | Duplicate of cppcoreguidelines variant |
| `modernize-use-trailing-return-type` | Style preference, not a bug |
| `modernize-use-auto` | Explicit types preferred for clarity |
| `modernize-use-scoped-lock` | `std::lock_guard` is sufficient for single-mutex locks |
| `modernize-avoid-c-arrays` | Required for ImGui API interop (char buffers) |
| `modernize-deprecated-headers` | `<signal.h>` needed for POSIX signals on Linux |
| `readability-identifier-naming` | Conflicts with different naming conventions |
| `readability-identifier-length` | Too restrictive (e.g., `i`, `it` are fine) |
| `readability-magic-numbers` | Too noisy for UI code (colors, layout constants) |
| `readability-implicit-bool-conversion` | Common C++ idiom (`if (ptr)`) |
| `readability-function-cognitive-complexity` | UI menu rendering inherently has nested conditionals |
| `readability-convert-member-functions-to-static` | Methods may need `this` in future |
| `readability-make-member-function-const` | Methods may mutate in future |
| `readability-use-concise-preprocessor-directives` | `#elifdef` not universally supported |
| `portability-avoid-pragma-once` | `#pragma once` is the project standard |
| `performance-unnecessary-value-param` | False positives with std::stop_token and similar |
| `cppcoreguidelines-avoid-c-arrays` | Required for ImGui API interop |
| `cppcoreguidelines-avoid-magic-numbers` | UI code has many layout constants |
| `cppcoreguidelines-non-private-member-variables-in-classes` | Conflicts with protected members in Layer base class |
| `cppcoreguidelines-pro-bounds-array-to-pointer-decay` | Required for C API interop (ImGui, snprintf) |
| `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` | Too verbose; bounds checked by logic |
| `cppcoreguidelines-pro-bounds-constant-array-index` | Too restrictive for loop-based array access |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic` | Required for std::from_chars and similar APIs |
| `cppcoreguidelines-pro-type-reinterpret-cast` | Required for OpenGL string queries (glGetString) |
| `cppcoreguidelines-pro-type-vararg` | Required for ImGui::Text and snprintf |

### Why Not Implemented
- **Toolchain files:** CMake Presets already provide complete build configurations
- **install/export config:** This is an app template, not a library - no downstream consumers
- **libc++:** Adds complexity; platform defaults work well and are more portable
- **CPM.cmake:** FetchContent is sufficient; CPM adds another dependency to manage

## When in Doubt

1. Check existing code for patterns and conventions
2. Run static analysis tools
3. Follow the principle of least surprise
4. Refer to C++ Core Guidelines for best practices

## Code Review Instructions

When performing a code review on this project:

1. **C++23 Compliance**: Flag any use of deprecated patterns when modern C++23 alternatives exist (e.g., prefer `std::ranges`, `std::views`, `std::string::contains()`)

2. **Memory Safety**: Check for potential memory issues - prefer smart pointers, RAII, avoid raw `new`/`delete`

3. **Thread Safety**: Verify proper mutex usage, check for race conditions, ensure `std::atomic` is used correctly

4. **Error Handling**: Ensure functions check return values and handle errors appropriately

5. **Naming Conventions**: Enforce project standards:
   - Classes: PascalCase
   - Functions: camelCase  
   - Members: m_camelCase
   - Constants: UPPER_SNAKE_CASE

6. **Include Order**: Verify includes follow project standard (matching header → project → third-party → stdlib)

7. **Performance**: Flag unnecessary copies, suggest `const&` or move semantics where appropriate

8. **Testing**: Suggest tests for new functionality, verify edge cases are covered
