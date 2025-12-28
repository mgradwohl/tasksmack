# Test Coverage Implementation Guide

**Quick Start**: This guide provides step-by-step instructions for implementing the test coverage improvements identified in the [test coverage analysis](test-coverage-analysis.md).

## Before You Start

1. Read the [executive summary](test-coverage-summary.md) for an overview
2. Review the [full analysis](test-coverage-analysis.md) for details
3. Ensure your development environment is set up per [CONTRIBUTING.md](../CONTRIBUTING.md)

## Priority 1: Security Fix (30 minutes)

### Issue: std::system() in ShellLayer

**File**: `src/App/ShellLayer.cpp:129`  
**Risk**: HIGH - Command injection vulnerability with user-controlled paths

**Current Code**:
```cpp
const std::string command = "xdg-open \"" + filePath.string() + "\" &";
const int result = std::system(command.c_str()); // NOLINT(concurrency-mt-unsafe)
```

**Recommended Fix**:

Create `src/Platform/IFileOpener.h`:
```cpp
#pragma once
#include <filesystem>

namespace Platform {
    class IFileOpener {
    public:
        virtual ~IFileOpener() = default;
        virtual bool openFileWithDefaultEditor(const std::filesystem::path& path) = 0;
    };

    std::unique_ptr<IFileOpener> makeFileOpener();
}
```

Implement platform-specific versions in `src/Platform/Linux/LinuxFileOpener.cpp` and `src/Platform/Windows/WindowsFileOpener.cpp`.

**Benefits**: Eliminates command injection, improves testability, follows architecture patterns.

## Priority 2: Core Layer Tests (1-2 days)

### Step 1: Create Core Test Directory

```bash
mkdir -p tests/Core
```

### Step 2: Create Application Tests

**File**: `tests/Core/test_Application.cpp`

Start with basic construction test:
```cpp
#include "Core/Application.h"
#include <gtest/gtest.h>

TEST(ApplicationTest, ConstructsWithDefaultSpec) {
    Core::ApplicationSpecification spec;
    Core::Application app(spec);

    EXPECT_EQ(&app, &Core::Application::get());
}
```

Add to `tests/CMakeLists.txt`:
```cmake
target_sources(TaskSmack_tests PRIVATE
    Core/test_Application.cpp
)
```

### Step 3: Create Window Tests

**File**: `tests/Core/test_Window.cpp`

Test basic window properties:
```cpp
#include "Core/Window.h"
#include <gtest/gtest.h>

TEST(WindowTest, GetSizeMatchesSpecification) {
    Core::WindowSpecification spec;
    spec.Width = 1024;
    spec.Height = 768;

    Core::Window window(spec);

    auto [w, h] = window.getSize();
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 768);
}
```

### Step 4: Run Tests

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -R "ApplicationTest|WindowTest"
```

## Priority 3: UserConfig Tests (2-3 days)

### Step 1: Create Test Infrastructure

**File**: `tests/App/test_UserConfig.cpp`

Set up test fixture with temporary config directory:
```cpp
#include "App/UserConfig.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

class UserConfigTest : public ::testing::Test {
protected:
    std::filesystem::path testConfigPath;

    void SetUp() override {
        testConfigPath = std::filesystem::temp_directory_path()
                       / "tasksmack_test"
                       / "config.toml";
        std::filesystem::create_directories(testConfigPath.parent_path());
    }

    void TearDown() override {
        std::filesystem::remove_all(testConfigPath.parent_path());
    }

    void writeConfigFile(const std::string& content) {
        std::ofstream file(testConfigPath);
        file << content;
    }
};
```

### Step 2: Add Edge Case Tests

**Test malformed TOML**:
```cpp
TEST_F(UserConfigTest, MalformedTOMLUsesDefaults) {
    writeConfigFile("invalid toml {{{");

    // UserConfig should not crash, should log error, use defaults
    // Note: Current UserConfig is singleton, may need refactoring for testability
}
```

**Test out-of-range values**:
```cpp
TEST_F(UserConfigTest, OutOfRangeValuesAreClamped) {
    writeConfigFile(R"(
[sampling]
interval_ms = -100
history_max_seconds = 999999
)");

    // Load and verify values are clamped
}
```

### Step 3: Test Save/Load Round-Trip

```cpp
TEST_F(UserConfigTest, SaveLoadPreservesSettings) {
    // Create settings, save, load, verify all fields match
}
```

## Priority 4: Code Quality Quick Wins (1 day)

### Quick Win 1: Add [[nodiscard]] Attributes

**File**: `src/Domain/BackgroundSampler.h`

**Before**:
```cpp
bool isRunning() const;
std::chrono::milliseconds interval() const;
```

**After**:
```cpp
[[nodiscard]] bool isRunning() const;
[[nodiscard]] std::chrono::milliseconds interval() const;
```

### Quick Win 2: Consolidate Constants

**File**: `src/App/ShellLayer.cpp`

**Before** (lines 46 and 75):
```cpp
constexpr std::array<int, 4> stops = {100, 250, 500, 1000};
// ... 30 lines later ...
constexpr std::array<int, 4> stops = {100, 250, 500, 1000};
```

**After** (add to namespace scope):
```cpp
namespace {
    constexpr std::array REFRESH_INTERVAL_STOPS = {100, 250, 500, 1000};

    // Use REFRESH_INTERVAL_STOPS in both functions
}
```

### Quick Win 3: Extract PDH Helper

**File**: `src/Platform/Windows/WindowsDiskProbe.cpp`

**Before** (repeated 5 times):
```cpp
const double value = counterValue.doubleValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
```

**After** (add helper at top of file):
```cpp
namespace {
    [[nodiscard]] inline double extractPdhDouble(const PDH_FMT_COUNTERVALUE& value) noexcept {
        return value.doubleValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }
}

// Use in 5 places:
const double readBytesPerSec = extractPdhDouble(counterValue);
```

### Quick Win 4: Add Numeric Tests

**File**: `tests/Domain/test_Numeric.cpp`

```cpp
#include "Domain/Numeric.h"
#include <gtest/gtest.h>
#include <limits>

TEST(NumericTest, NarrowOrOverflowReturnsDefault) {
    std::int64_t large = std::numeric_limits<std::int64_t>::max();
    int result = Domain::Numeric::narrowOr<int>(large, 99);
    EXPECT_EQ(result, 99);
}

TEST(NumericTest, NarrowOrWithinRangeReturnsValue) {
    std::int64_t value = 42;
    int result = Domain::Numeric::narrowOr<int>(value, 99);
    EXPECT_EQ(result, 42);
}
```

## Priority 5: UI Layer Tests (2-3 days)

### Step 1: ThemeLoader Hex Parsing Tests

**File**: `tests/UI/test_ThemeLoader.cpp` (extend existing)

```cpp
TEST(ThemeLoaderTest, HexToImVec4EightDigitWithAlpha) {
    auto color = UI::ThemeLoader::hexToImVec4("#FF408180");
    EXPECT_FLOAT_EQ(color.x, 1.0f);      // Red
    EXPECT_FLOAT_EQ(color.y, 0.25f);     // Green
    EXPECT_FLOAT_EQ(color.z, 0.505f);    // Blue
    EXPECT_FLOAT_EQ(color.w, 0.502f);    // Alpha
}

TEST(ThemeLoaderTest, HexToImVec4InvalidLengthReturnsError) {
    auto color = UI::ThemeLoader::hexToImVec4("#FF");
    // Should return error color, not crash
}
```

### Step 2: Theme File Loading Tests

```cpp
TEST(ThemeLoaderTest, LoadThemeFromValidFile) {
    // Create temp TOML with complete theme
    // Load theme
    // Verify all colors parsed correctly
}
```

## Testing Best Practices

### 1. Use Descriptive Test Names

**Good**:
```cpp
TEST(ProcessModelTest, RefreshWithZeroProcessesReturnsEmptySnapshot)
TEST(UserConfigTest, MalformedTOMLLogsErrorAndUsesDefaults)
```

**Bad**:
```cpp
TEST(ProcessModelTest, Test1)
TEST(UserConfigTest, LoadTest)
```

### 2. Follow Arrange-Act-Assert Pattern

```cpp
TEST(HistoryTest, PushBeyondCapacityDropsOldest) {
    // Arrange
    Domain::History<int, 3> history;

    // Act
    history.push(1);
    history.push(2);
    history.push(3);
    history.push(4);  // Should drop 1

    // Assert
    EXPECT_EQ(history.size(), 3);
    EXPECT_EQ(history[0], 2);  // Oldest remaining
}
```

### 3. Test One Thing Per Test

**Good** (focused):
```cpp
TEST(BackgroundSamplerTest, StartSetsRunningFlag) {
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    sampler.start();

    EXPECT_TRUE(sampler.isRunning());
}
```

### 4. Use Fixtures for Common Setup

```cpp
class ProcessModelTest : public ::testing::Test {
protected:
    std::unique_ptr<MockProcessProbe> probe;

    void SetUp() override {
        probe = std::make_unique<MockProcessProbe>();
        probe->withProcess(1, "test").withCpuTime(1, 1000, 500);
    }
};

TEST_F(ProcessModelTest, InitialRefreshPopulatesSnapshots) {
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.snapshots().size(), 1);
}
```

## Running Tests Selectively

### Run specific test file:
```bash
ctest --preset debug -R "UserConfig"
```

### Run specific test case:
```bash
ctest --preset debug -R "UserConfigTest.MalformedTOML"
```

### Run tests with verbose output:
```bash
ctest --preset debug -V
```

### Run tests with AddressSanitizer:
```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

## Tracking Progress

### 1. Update Test Coverage Regularly

Run coverage script after adding tests:
```bash
./tools/coverage.sh --open
```

### 2. Check Coverage by Layer

Look at HTML report in `coverage/index.html`:
- Platform: Target 90%+
- Domain: Target 85%+
- UI: Target 50%+ (harder due to ImGui)
- App: Target 70%+
- Core: Target 60%+ (harder due to GLFW)

### 3. Document New Tests

Add to this file or create layer-specific test guides.

## Getting Help

- **Architecture questions**: See [tasksmack.md](../tasksmack.md)
- **Build/test setup**: See [CONTRIBUTING.md](../CONTRIBUTING.md)
- **Test examples**: See [test-coverage-analysis.md](test-coverage-analysis.md)
- **Existing tests**: Review `tests/Domain/` and `tests/Platform/` for patterns

## Next Steps

1. Start with Priority 1 (security fix) - **30 minutes**
2. Implement Priority 2 (Core tests) - **1-2 days**
3. Tackle Priority 3 (UserConfig tests) - **2-3 days**
4. Apply Priority 4 (quick wins) - **1 day**
5. Continue with Priority 5 (UI tests) - **2-3 days**

Track your progress by creating GitHub issues for each priority item and referencing this guide.

---

**Remember**: Small, focused tests are better than large, complex ones. Start simple and build up coverage incrementally.
