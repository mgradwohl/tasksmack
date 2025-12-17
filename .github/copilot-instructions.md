# TaskSmack - Copilot Instructions

Cross-platform system monitor (C++23 / Clang 21+ / OpenGL / ImGui). Strict layered architecture: **Platform → Domain → UI**.

> **Related Docs:** [README.md](../README.md) (build/tools), [tasksmack.md](../tasksmack.md) (architecture vision), [process.md](../process.md) (implementation details), [CONTRIBUTING.md](../CONTRIBUTING.md) (PR process), [TODO.md](../TODO.md) (backlog)

## Quick Reference

**Build/Test/Tools**: See [README.md](../README.md) sections:
- Build commands → "Getting Started" and "Build Types"  
- Prerequisites → "Requirements"
- Formatting/linting → "Code Quality Tools"

**Essential commands:**
```bash
# Format before commit (REQUIRED)
./tools/clang-format.sh        # Linux
pwsh tools/clang-format.ps1    # Windows

# Lint regularly
./tools/clang-tidy.sh debug    # Linux
pwsh tools/clang-tidy.ps1 debug # Windows

# Test after changes
ctest --preset debug            # Linux
ctest --preset win-debug        # Windows
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

### Rule of 5 / RAII Compliance
- **Default behavior**: If a class needs no special resource management, use compiler-generated defaults
- **Rule of 5**: If you define/delete any of: destructor, copy constructor, copy assignment, move constructor, move assignment → define or delete all five
- **RAII**: Acquire resources in constructor, release in destructor. Never use manual `new`/`delete` in application code
- **Smart pointers**: Use `std::unique_ptr` for exclusive ownership, `std::shared_ptr` for shared ownership
- **Example violating Rule of 5**:
  ```cpp
  // BAD: Custom destructor but compiler-generated copy ops (double-free risk)
  class Resource {
      int* data;
  public:
      Resource() : data(new int[100]) {}
      ~Resource() { delete[] data; }
      // Missing: copy/move constructors and assignment operators
  };
  ```
- **Example following Rule of 5**:
  ```cpp
  // GOOD: All five explicitly handled
  class Resource {
      std::unique_ptr<int[]> data;
  public:
      Resource() : data(std::make_unique<int[]>(100)) {}
      // Compiler-generated destructor, copy/move ops work correctly
      // Or explicitly delete copy ops if non-copyable:
      Resource(const Resource&) = delete;
      Resource& operator=(const Resource&) = delete;
      Resource(Resource&&) = default;
      Resource& operator=(Resource&&) = default;
  };
  ```

### Exception Safety
- **Prefer RAII** over manual cleanup (destructors run automatically during stack unwinding)
- **Constructors**: Use member initializer lists; if allocation can throw, ensure no leaks
- **No-throw operations**: Mark with `noexcept` (destructors, move constructors, swap)
- **Resource acquisition**: Use smart pointers or standard containers; avoid bare `new`
- **clang-tidy checks**: `bugprone-exception-escape` warns about exceptions escaping `noexcept` functions

### Math Expression Clarity
- Always add parentheses to clarify operator precedence in math expressions
- Example: `a + (b * c)` instead of `a + b * c`
- This improves readability and avoids `readability-math-missing-parentheses` warnings

### Preprocessor Directives
- Use `#ifdef X` instead of `#if defined(X)` for simple checks
- Use `#ifndef X` instead of `#if !defined(X)`
- Compound conditions like `#if defined(X) && !defined(Y)` stay as-is

### Avoiding Common clang-tidy Warnings
- **`cppcoreguidelines-pro-type-member-init`**: Initialize all member variables in constructor initializer list or with in-class defaults
- **`cppcoreguidelines-pro-type-reinterpret-cast`**: Avoid `reinterpret_cast`; use type-safe alternatives
- **`cppcoreguidelines-owning-memory`**: Use smart pointers; avoid raw `new`/`delete`
- **`modernize-use-override`**: Always use `override` keyword on virtual function overrides
- **`modernize-use-nullptr`**: Use `nullptr` instead of `NULL` or `0` for pointers
- **`readability-const-return-type`**: Don't return `const` by value (prevents move optimization)
- **`performance-unnecessary-copy-initialization`**: Use `const&` or `&&` to avoid copies
- **`bugprone-unchecked-optional-access`**: Check `std::optional` with `has_value()` before accessing

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
- ❌ Violating Rule of 5 (custom destructor without handling copy/move)
- ❌ Using raw `new`/`delete` instead of smart pointers
- ❌ Forgetting to initialize member variables (causes `cppcoreguidelines-pro-type-member-init` warnings)
- ❌ Missing `override` keyword on virtual function overrides

## Engineering Workflow

- When modifying project structure, scripts, clang configs, or CMake files → update `README.md` and this file
- New folders under project root → consider `.gitignore`, exclude from clang-format/tidy configs
- New dependencies → use CMake FetchContent with `SYSTEM` keyword, document in `README.md`
- **GLAD dependency:** Requires Python 3 + jinja2 at build time for OpenGL loader generation

---
*For comprehensive build instructions, prerequisites, tools, and project structure, see [README.md](../README.md).*

*For PR process and contribution workflow, see [CONTRIBUTING.md](../CONTRIBUTING.md).*

---

## Code Review Instructions

When performing a code review on this project:

1. **C++23 Compliance**: Flag any use of deprecated patterns when modern C++23 alternatives exist (e.g., prefer `std::ranges`, `std::views`, `std::string::contains()`)

2. **Memory Safety**: Check for potential memory issues - prefer smart pointers, RAII, avoid raw `new`/`delete`

3. **Rule of 5**: Verify classes with custom destructors properly handle copy/move operations (define or delete all five)

4. **Exception Safety**: Check RAII compliance, `noexcept` on move/swap/destructors, no resource leaks on exception paths

5. **Thread Safety**: Verify proper mutex usage, check for race conditions, ensure `std::atomic` is used correctly

6. **Architecture Boundaries**: Verify layer dependencies are correct:
   - Platform probes should return raw counters, not computed values
   - Domain should not depend on UI, Core, or graphics libraries  
   - UI should not call Platform probes directly
   - All OpenGL calls should be in UI/Core layers only

7. **Naming Conventions**: Enforce project standards (PascalCase classes, camelCase functions, m_camelCase members)

8. **Include Order**: Verify includes follow project standard (matching header → project → third-party → stdlib)

9. **clang-tidy Compliance**: Check for warnings that indicate common issues (uninitialized members, missing `override`, unnecessary copies)
