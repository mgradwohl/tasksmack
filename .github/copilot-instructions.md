# TaskSmack - Copilot Instructions

Cross-platform system monitor (C++23 / Clang 21+ / OpenGL / ImGui). Strict layered architecture: **Platform → Domain → UI**.

> **Related Docs:** [README.md](../README.md) (build/tools), [tasksmack.md](../tasksmack.md) (architecture vision), [process.md](../process.md) (implementation details), [CONTRIBUTING.md](../CONTRIBUTING.md) (PR process), [TODO.md](../TODO.md) (backlog)

## Quick Reference

```bash
# Build (Windows)          # Build (Linux)
cmake --preset win-debug   cmake --preset debug
cmake --build --preset win-debug  cmake --build --preset debug

# Test
ctest --preset win-debug   # or: ctest --preset debug

# Format & Lint (before commit)
pwsh tools/clang-format.ps1    # or: ./tools/clang-format.sh
pwsh tools/clang-tidy.ps1      # or: ./tools/clang-tidy.sh

# Check prerequisites
pwsh tools/check-prereqs.ps1   # or: ./tools/check-prereqs.sh
```

## Architecture (Critical)

```
App (ShellLayer, Panels)
    ↓
UI (ImGui/ImPlot integration)
    ↓
Core (Application loop, Window, GLFW)
    ↓
Domain (ProcessModel, SystemModel, History)
    ↓
Platform (OS-specific probes)
    ↓
OS APIs (Linux: /proc/*, Windows: NtQuery*)
```

**Layer rules:**
- **Platform** (`src/Platform/`): Stateless probes returning raw counters (CPU ticks, bytes). No computation.
- **Domain** (`src/Domain/`): Computes deltas/rates from counters, maintains history, publishes immutable snapshots.
- **UI** (`src/App/`, `src/UI/`): Renders snapshots via ImGui. Never calls Platform directly.
- **Core** (`src/Core/`): Application lifecycle, windowing, event dispatch.
- OpenGL calls confined to `Core/` and `UI/` only. Domain/Platform are graphics-agnostic.
- Domain and Platform never depend on UI, Core, or graphics libraries.

## Key Patterns

### Probe → Domain Flow
Probes return **raw counters**, domain computes **deltas and rates**:

```cpp
// Platform: raw counters (stateless read from OS)
ProcessCounters{userTime=1000, systemTime=500, startTimeTicks=12345}

// Domain: compute delta from previous snapshot
uint64_t uniqueKey = hash(pid, startTime);  // Handle PID reuse
auto prev = m_PrevCounters[uniqueKey];
cpuPercent = (delta_user + delta_system) / totalCpuDelta * 100;

// UI: render ProcessSnapshot.cpuPercent
```

**Why this separation?**
- Testability: Domain calculations can be unit tested with mock probes
- No `#ifdef` soup: Platform-specific code isolated to probe implementations
- Consistent semantics: CPU%, rates calculated the same way on all platforms

### Capability Reporting
Probes report what their OS supports; UI degrades gracefully:

```cpp
struct ProcessCapabilities {
    bool hasIoCounters = false;    // Linux: /proc/[pid]/io
    bool hasThreadCount = false;
    bool hasStartTime = true;
};
```

### Background Sampling
- Use `BackgroundSampler` with `std::jthread` + `std::stop_token`
- UI registers callback; sampler delivers data on background thread
- Domain models are thread-safe (`std::shared_mutex`)
- Sampler interval configurable (default 1 second)

### Panel Lifecycle
```cpp
void onAttach() override { /* create model, start sampler */ }
void onDetach() override { /* stop sampler, cleanup */ }
void render(bool* open) override { /* ImGui::Begin/End, consume snapshots */ }
```

## Coding Standards

### Language & Style
- **C++ Standard:** C++23 (required)
- **Compiler:** Clang 21+ with lld linker
- **Formatting:** `.clang-format` (LLVM base, Allman braces) - run before commits
- **Static Analysis:** `.clang-tidy` - run regularly

### Naming
- Classes: `PascalCase` | Functions: `camelCase` | Members: `m_camelCase` | Constants: `UPPER_SNAKE_CASE`

### Includes (order matters)
1. Matching header (`.cpp` files only)
2. Project headers (`src/`, `tests/`), alphabetical
3. Third-party (`<spdlog/...>`, `<imgui.h>`), alphabetical
4. Standard library (`<memory>`, `<vector>`), alphabetical

Separate each group with a blank line. Use `#pragma once` in all headers.

### GLFW/GLAD Header Order
```cpp
// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// clang-format on
#include <glad/gl.h>
```

### Modern C++23 Features (Prefer)
- `std::string::contains()`, `std::string::starts_with()` over `find()`
- `std::ranges` and `std::views` over raw loops
- `std::format` or `std::print` for type-safe formatting
- `std::jthread` with `std::stop_token` for background threads

### Math Expression Clarity
- Always add parentheses to clarify operator precedence in math expressions
- Example: `a + (b * c)` instead of `a + b * c`
- This improves readability and avoids `readability-math-missing-parentheses` warnings

### Preprocessor Directives
- Use `#ifdef X` instead of `#if defined(X)` for simple checks
- Use `#ifndef X` instead of `#if !defined(X)`
- Compound conditions like `#if defined(X) && !defined(Y)` stay as-is

## Testing

- Framework: Google Test in `tests/` mirroring `src/` structure
- Mocks: `tests/Mocks/MockProbes.h` for `IProcessProbe`, `ISystemProbe`
- Use `EXPECT_DOUBLE_EQ` for floats, not `EXPECT_EQ`
- Define mocks outside anonymous namespace when using `std::make_unique`

### Test Organization
```cpp
namespace Domain {
namespace {

// ========== Basic Operations ==========
TEST(ProcessModelTest, InitialState) { /* ... */ }

// ========== CPU Calculation ==========
TEST(ProcessModelTest, CpuPercentFromDeltas) { /* ... */ }

} // namespace
} // namespace Domain
```

### Coverage
```bash
pwsh tools/coverage.ps1    # Generates coverage/index.html
```

## Adding New Features

1. **New probe**: `Platform/IXxxProbe.h` (interface) → `Platform/Linux/LinuxXxxProbe.cpp` (impl)
2. **New model**: `Domain/XxxModel.cpp` (computes deltas from probe counters)
3. **New panel**: `App/Panels/XxxPanel.cpp` (owns model + sampler)
4. Update `CMakeLists.txt` (`TASKSMACK_SOURCES`), add tests, run clang-format/tidy

## Common Pitfalls

- ❌ Computing CPU% in Platform probes (belongs in Domain)
- ❌ Calling probes from UI thread (use Domain models)
- ❌ Forgetting `GLFW_INCLUDE_NONE` before GLFW headers
- ❌ `EXPECT_EQ` on floats (use `EXPECT_DOUBLE_EQ`)
- ❌ Mocks inside anonymous namespace with `make_unique`
- ❌ Using `using namespace std` in headers
- ❌ Ignoring clang-tidy warnings (CI will fail)
- ❌ Committing without running clang-format
- ❌ Adding dependencies without `SYSTEM` keyword in FetchContent

## Engineering Workflow

- When modifying project structure, scripts, clang configs, or CMake files → update `README.md` and this file
- New folders under project root → consider `.gitignore`, exclude from clang-format/tidy configs
- New dependencies → use CMake FetchContent with `SYSTEM` keyword, document in `README.md`
- **GLAD dependency:** Requires Python 3 + jinja2 at build time for OpenGL loader generation

---
*Extended reference below. For detailed patterns, see linked docs above.*

---

## Extended Reference

### Build Commands (Full)

```bash
# Linux presets
cmake --preset debug|relwithdebinfo|release|optimized|coverage|asan-ubsan|tsan
cmake --build --preset <name>

# Windows presets  
cmake --preset win-debug|win-relwithdebinfo|win-release|win-optimized|win-coverage
cmake --build --preset <name>
```

### Project Structure

```
src/
├── Core/           # Application loop, Window, Layer base
├── UI/             # ImGui/OpenGL integration (UILayer)
├── App/            # ShellLayer + Panels/
├── Domain/         # ProcessModel, SystemModel, BackgroundSampler, History<T,N>
└── Platform/       # IProcessProbe, ISystemProbe, Linux/ implementations

tests/
├── Domain/         # test_ProcessModel.cpp, test_SystemModel.cpp, etc.
└── Mocks/          # MockProbes.h (shared test mocks)
```

### ImGui Patterns

- Panels inherit `Panel` base class with `onAttach()`, `onDetach()`, `onUpdate()`, `render()`
- Use `ImGui::BeginTable()` with `ImGuiTableFlags_Sortable` for data grids
- ShellLayer manages panel visibility and docking via `setupDockspace()`

### Concurrency

```cpp
// Background sampler pattern
std::jthread m_SamplerThread;
std::atomic<bool> m_Running{false};
std::mutex m_CallbackMutex;

void samplerLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        auto data = m_Probe->enumerate();
        { std::lock_guard lock(m_CallbackMutex); if (m_Callback) m_Callback(data); }
        std::this_thread::sleep_for(m_Config.interval);
    }
}
```

### Adding Dependencies

```cmake
FetchContent_Declare(mylib
    GIT_REPOSITORY https://github.com/example/mylib.git
    GIT_TAG v1.0.0
    SYSTEM  # Suppress warnings from third-party code
)
FetchContent_MakeAvailable(mylib)
```

Update: `CMakeLists.txt`, `README.md`, `.clang-tidy` exclusions if needed.

### Code Review Checklist

1. Layer boundaries respected (Platform=raw counters, Domain=computation, UI=rendering)
2. Thread safety: `std::shared_mutex` for models, `std::atomic` for flags
3. Modern C++23: `std::string::contains()`, `std::ranges`, `std::format`
4. Include order correct, `GLFW_INCLUDE_NONE` present
5. Tests use mocks, `EXPECT_DOUBLE_EQ` for floats

## Code Review Instructions

When performing a code review on this project:

1. **C++23 Compliance**: Flag any use of deprecated patterns when modern C++23 alternatives exist (e.g., prefer `std::ranges`, `std::views`, `std::string::contains()`)

2. **Memory Safety**: Check for potential memory issues - prefer smart pointers, RAII, avoid raw `new`/`delete`

3. **Thread Safety**: Verify proper mutex usage, check for race conditions, ensure `std::atomic` is used correctly

4. **Architecture Boundaries**: Verify layer dependencies are correct:
   - Platform probes should return raw counters, not computed values
   - Domain should not depend on UI, Core, or graphics libraries  
   - UI should not call Platform probes directly
   - All OpenGL calls should be in UI/Core layers only

5. **Naming Conventions**: Enforce project standards (PascalCase classes, camelCase functions, m_camelCase members)

6. **Include Order**: Verify includes follow project standard (matching header → project → third-party → stdlib)
