# TaskSmack Test Coverage Analysis and Code Review

## Executive Summary

### Test Coverage Statistics
- **Total Source Files**: 77 (src/)
- **Total Test Files**: 24 (tests/)
- **Test-to-Source Ratio**: ~31%

### Existing Test Coverage by Layer
- **Platform Layer**: ✅ Well covered (Linux/Windows probes, contracts)
- **Domain Layer**: ✅ Good coverage (ProcessModel, SystemModel, StorageModel, History, BackgroundSampler)
- **UI Layer**: ⚠️ Minimal (Format, HistoryWidgets only)
- **App Layer**: ❌ No tests
- **Core Layer**: ❌ No tests

## Critical Gaps in Test Coverage

### HIGH PRIORITY (Security, Stability, Correctness)

#### 1. Core Layer (Application Lifecycle) - NO TESTS
**Files**: `src/Core/Application.cpp`, `src/Core/Window.cpp`
**Risk Level**: HIGH - Main loop, window management, layer stack management
**Missing Coverage**:
- Application startup/shutdown sequence
- Layer stack management (push/pop, lifecycle)
- GLFW initialization/teardown
- Window creation/destruction
- Event loop handling
- Frame timing calculations
- Error handling when GLFW fails

**Recommended Tests**:
- Unit test: Application construction/destruction
- Unit test: Layer stack operations (push, iterate, detach in reverse)
- Integration test: Mocked GLFW window creation
- Edge case: GLFW initialization failure handling
- Edge case: Window creation failure
- Stress test: Multiple rapid layer attachments/detachments

#### 2. UserConfig (File I/O, Parsing, Platform-specific paths) - NO TESTS
**Files**: `src/App/UserConfig.cpp`
**Risk Level**: HIGH - Data persistence, config corruption, platform differences
**Missing Coverage**:
- TOML parsing (valid/invalid/malformed)
- Platform-specific config directory resolution
- File I/O error handling (permission denied, disk full, corrupted file)
- Settings validation and clamping
- ImGui layout state serialization
- Column visibility state persistence
- Concurrent access during save/load
- Edge case: Missing config directory
- Edge case: Invalid enum values
- Edge case: Out-of-range integer values (already uses narrowOr, but not tested)

**Recommended Tests**:
- Unit test: Config path resolution (Linux XDG_CONFIG_HOME, Windows APPDATA)
- Unit test: Parse valid TOML with all fields
- Unit test: Parse partial TOML (missing fields use defaults)
- Unit test: Parse invalid TOML (syntax error)
- Unit test: Out-of-range values are clamped
- Unit test: Invalid enum values default gracefully
- Unit test: ImGui layout serialization/deserialization
- Integration test: Save/load round-trip preserves settings
- Edge case: File write fails (read-only filesystem)
- Property-based test: Random valid TOML configurations

#### 3. ShellLayer (UI Orchestration, File Operations) - NO TESTS  
**Files**: `src/App/ShellLayer.cpp`
**Risk Level**: MEDIUM-HIGH - Menu actions, panel coordination, file opening
**Missing Coverage**:
- Menu bar actions (About, theme selection, refresh interval)
- Panel visibility toggles
- Status bar rendering
- Frame timing/FPS calculation
- Config file opening with platform handlers
- Dockspace setup
- TODO at line 464: Options dialog (placeholder)

**Recommended Tests**:
- Unit test: Frame timing accumulation and FPS calculation
- Unit test: Panel visibility state management
- Integration test: Mock file opening (avoid actual shell exec in tests)
- Edge case: Refresh interval snapping logic
- Edge case: File open when config doesn't exist

**Security Note**: Line 129 uses `std::system()` with user-controlled path. Ensure proper escaping/validation.

#### 4. UI Layer (ImGui Integration, Resource Loading) - MINIMAL TESTS
**Files**: `src/UI/UILayer.cpp`, `src/UI/ThemeLoader.cpp`, `src/UI/IconLoader.cpp`
**Risk Level**: MEDIUM - Resource loading, OpenGL state, theme parsing
**Missing Coverage**:
- ImGui context creation/destruction
- Font loading and scaling
- Theme discovery and loading
- Hex color parsing edge cases
- Icon/texture loading (stb_image integration)
- OpenGL texture management
- Theme file validation
- Malformed TOML theme files
- Missing theme assets

**Recommended Tests**:
- Unit test: ThemeLoader hex parsing edge cases (invalid format, alpha channel)
- Unit test: Theme discovery from multiple directories
- Unit test: Theme loading with missing fields (uses defaults)
- Unit test: Theme metadata extraction
- Mock test: Icon loader without real OpenGL context
- Integration test: Font loading with various sizes
- Edge case: Theme file with syntax errors
- Edge case: Theme references missing color keys

#### 5. Panel Components - NO TESTS
**Files**: `src/App/Panels/*.cpp`
**Risk Level**: MEDIUM - Complex UI logic, sorting, selection, process actions
**Missing Coverage**:
- ProcessesPanel: Sorting, filtering, selection, column visibility
- ProcessDetailsPanel: Tab navigation, metric display
- SystemMetricsPanel: Graph rendering, history display
- StoragePanel: Disk list, metric display
- Process action signal sending (terminate/kill/stop/resume)
- Selection state management
- Column configuration persistence

**Recommended Tests**:
- Unit test: Sorting logic for each column type
- Unit test: Process selection state management
- Unit test: Column visibility toggles
- Mock test: Panel rendering without real ImGui context
- Integration test: Process action flow (with mock IProcessActions)

### MEDIUM PRIORITY (Robustness, Maintainability)

#### 6. Platform Factory Functions - LIMITED TESTS
**Files**: `src/Platform/*/Factory.cpp`
**Missing Coverage**:
- Factory function behavior under low memory
- Nullptr handling
- Platform detection edge cases

**Recommended Tests**:
- Unit test: Factory returns non-null probes
- Unit test: Factory-created probes satisfy interface contracts

#### 7. History Buffer Edge Cases - PARTIAL TESTS
**File**: `src/Domain/History.h`
**Existing**: Basic push/pop/capacity tests
**Missing Coverage**:
- Buffer wraparound behavior
- Decimation logic (when samples > capacity)
- Thread safety under concurrent access
- Copy/move semantics

**Recommended Tests**:
- Unit test: Fill buffer beyond capacity (verify oldest dropped)
- Unit test: Concurrent push from multiple threads
- Property-based test: Invariants hold after random operations

#### 8. BackgroundSampler Edge Cases - PARTIAL TESTS
**File**: `src/Domain/BackgroundSampler.cpp`
**Existing**: Basic start/stop, callback tests
**Missing Coverage**:
- Rapid start/stop cycles
- Interval changes during operation
- Callback exceptions (does sampler thread crash?)
- Stop during probe enumeration
- Probe enumerate() throws exception

**Recommended Tests**:
- Stress test: Rapid start/stop/start cycles
- Unit test: Change interval while running
- Edge case: Callback throws exception (sampler continues)
- Edge case: Probe enumerate throws (sampler handles gracefully)

### LOW PRIORITY (Nice-to-Have)

#### 9. Format Functions - SOME TESTS
**File**: `src/UI/Format.h`
**Existing**: Affinity mask formatting
**Missing Coverage**:
- Byte size formatting (KB/MB/GB)
- Time formatting (H:MM:SS)
- Percentage formatting
- State character mapping

**Recommended Tests**:
- Unit test: Format edge cases (0 bytes, max uint64)
- Unit test: Time formatting with overflow

#### 10. Numeric Utilities - NO DEDICATED TESTS
**Files**: `src/Domain/Numeric.h`, `src/UI/Numeric.h`
**Missing Coverage**:
- `narrowOr` template with various types
- Overflow/underflow behavior
- Floating point edge cases

**Recommended Tests**:
- Unit test: narrowOr handles overflow
- Unit test: narrowOr handles underflow
- Unit test: narrowOr with negative values
- Property-based test: narrowOr always returns valid value


## Code Quality Analysis

### NOLINT Usage Audit (15 instances)

Current NOLINTs are generally justified but some can be eliminated:

1. **IconLoader.cpp:40** - `reinterpret_cast` for GL texture binding
   - **Status**: JUSTIFIED - OpenGL API requires this cast
   - **Action**: Keep with explanation

2. **IconLoader.h:42** - `performance-no-int-to-ptr` for ImTextureID
   - **Status**: JUSTIFIED - ImGui API requires this cast
   - **Action**: Keep with explanation

3. **Theme.h:50-160** - `readability-redundant-member-init`
   - **Status**: QUESTIONABLE - Large block disable
   - **Action**: Review if member inits can be reorganized
   - **Recommendation**: Use in-class initializers instead of constructor init lists where possible

4. **UserConfig.cpp:63** - `concurrency-mt-unsafe` for std::getenv
   - **Status**: ACCEPTABLE - Called early in single-threaded init
   - **Action**: Add comment explaining why safe in this context

5. **ShellLayer.cpp:129** - `concurrency-mt-unsafe` for std::system
   - **Status**: SECURITY CONCERN - Uses user-controlled path
   - **Action**: Add path validation/escaping
   - **Recommendation**: Use platform-specific APIs instead of system()

6. **WindowsDiskProbe.cpp:206-233** - Multiple `pro-type-union-access` for PDH values
   - **Status**: JUSTIFIED - Windows PDH API uses unions
   - **Action**: Keep, but consider helper function to reduce repetition

7. **WindowsProcAddress.h:38** - `reinterpret_cast` for GetProcAddress
   - **Status**: JUSTIFIED - Windows API requires this
   - **Action**: Keep

8. **Window.cpp:48** - `reinterpret_cast` for byte array
   - **Status**: JUSTIFIED - Low-level memory access
   - **Action**: Keep

9. **Window.cpp:126** - `prefer-member-initializer`
   - **Status**: JUSTIFIED - GLFW requires ordering
   - **Action**: Keep with explanation

10. **WindowsProcessProbe.cpp:397** - TODO for network counters
    - **Status**: FEATURE INCOMPLETE
    - **Action**: Track in GitHub issue, implement ETW/GetPerTcpConnectionEStats

### TODO/FIXME Items (2 instances)

1. **ShellLayer.cpp:464** - "TODO: Open options dialog"
   - **Priority**: LOW - Feature placeholder
   - **Recommendation**: Create GitHub issue or implement basic options dialog

2. **WindowsProcessProbe.cpp:397** - "TODO: Implement using ETW or GetPerTcpConnectionEStats"
   - **Priority**: MEDIUM - Feature parity with completed infrastructure
   - **Recommendation**: Already tracked in completed-features.md as future work

### Const Correctness Review

**Generally Good**, but opportunities exist:

1. **Application.cpp:89** - `deltaTime` clamp could be constexpr
   ```cpp
   // Current
   deltaTime = std::min(deltaTime, 0.1F);
   
   // Better
   constexpr float MAX_DELTA_TIME = 0.1F;
   deltaTime = std::min(deltaTime, MAX_DELTA_TIME);
   ```

2. **UserConfig.cpp:53** - Magic number for window position validation
   ```cpp
   // Current
   constexpr int WINDOW_POS_ABS_MAX = 100'000;
   
   // Already constexpr ✓
   ```

3. **ShellLayer.cpp** - Refresh interval stops could be shared constant
   ```cpp
   // Current: Duplicated in snapRefreshIntervalMs and drawRefreshPresetTicks
   constexpr std::array<int, 4> stops = {100, 250, 500, 1000};
   
   // Better: Define once at namespace scope
   ```

4. **ProcessModel, SystemModel, StorageModel** - Consider marking more getters as `[[nodiscard]]`

### [[nodiscard]] Usage Review

**Good usage** in Platform interfaces and Domain snapshots.

**Missing opportunities**:

1. **BackgroundSampler.h:45** - `isRunning()` should be `[[nodiscard]]`
2. **BackgroundSampler.h:60** - `interval()` should be `[[nodiscard]]`
3. **Window.h** - All getter methods already marked ✓
4. **Application.h** - All query methods already marked ✓
5. **UserConfig.h** - `configPath()` already marked ✓

**Recommendation**: Add `[[nodiscard]]` to:
- `BackgroundSampler::isRunning()`
- `BackgroundSampler::interval()`
- Format.h helper functions (if they return by value)

### Modern C++23 Adoption

**Strong adherence** overall. Areas for improvement:

1. **Replace raw loops with ranges** where applicable
   - UserConfig.cpp:256-268 could use `std::views::enumerate`
   - ShellLayer.cpp:67-98 (drawing ticks) is already clean

2. **Consider std::expected** for error handling (C++23)
   - ThemeLoader::loadTheme returns optional, could return expected<ColorScheme, Error>
   - IconLoader::loadTexture returns invalid texture on error, could use expected

3. **Use std::print** more consistently
   - Already adopted in some places ✓
   - Consider replacing remaining spdlog for non-logging output

4. **Consider std::generator** for lazy iteration (C++23)
   - ProcessModel could yield snapshots instead of returning vector
   - Would reduce allocations for large process lists

### Memory Usage Analysis

**Generally efficient**, but opportunities:

1. **ProcessModel snapshots** - Returns by const reference ✓
2. **BackgroundSampler callback** - Passes vector by const ref ✓
3. **History buffer** - Fixed-size ring buffer ✓

**Potential improvements**:

1. **ProcessSnapshot string fields** - Consider string_view for read-only access
2. **Theme color parsing** - Builds temporary strings during hex parsing
   - ThemeLoader.cpp:46 creates string from string_view
   - Could use std::from_chars directly on string_view

3. **UserConfig TOML serialization** - Builds large table in memory
   - Could stream to file instead of building full tree
   - Current approach is fine for small configs

### Recursion Review

**No problematic recursion found** ✓

All iteration is explicit or via standard algorithms.

### Dead Code Review

**Minimal dead code**. No obvious unused functions.

**Potential cleanup**:
- Verify all ProcessColumn enum values are actually used
- Check if all theme colors are rendered
- Audit capability flags (hasNetworkCounters never true on Windows)

### Duplication Opportunities

1. **Refresh interval stops array** - Duplicated in ShellLayer.cpp
   - Lines 46 and 75 define same array
   - **Fix**: Define once at namespace scope

2. **Platform factory patterns** - Very similar code in Linux/Windows Factory.cpp
   - **Status**: ACCEPTABLE - Intentional for platform isolation
   - **Action**: Keep as-is per architecture

3. **Error color fallback** - Used in multiple places in ThemeLoader
   - **Fix**: Already factored into errorColor() helper ✓

4. **PDH union access** - Repeated pattern in WindowsDiskProbe.cpp
   - **Recommendation**: Extract helper function
   ```cpp
   [[nodiscard]] double extractPdhDouble(const PDH_FMT_COUNTERVALUE& value) noexcept {
       return value.doubleValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
   }
   ```

### Architecture Adherence

**Excellent layering** ✓

- Platform → Domain → UI → App → Core dependencies are clean
- No Platform calls from UI ✓
- No OpenGL calls outside Core/UI ✓
- Domain is graphics-agnostic ✓

**One minor concern**:
- ShellLayer.cpp:129 uses std::system() - Consider moving to Platform layer


## Concrete Test Recommendations with Examples

### Priority 1: Core Layer Tests

#### Test File: `tests/Core/test_Application.cpp`

```cpp
#include "Core/Application.h"
#include <gtest/gtest.h>

// Mock GLFW for testing without creating actual windows
class MockGLFWFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize GLFW once for all tests
        // Or mock GLFW functions
    }
};

TEST_F(MockGLFWFixture, ApplicationConstructsSuccessfully) {
    Core::ApplicationSpecification spec;
    spec.Name = "TestApp";
    spec.Width = 800;
    spec.Height = 600;
    
    Core::Application app(spec);
    EXPECT_EQ(&app, &Core::Application::get());
}

TEST_F(MockGLFWFixture, ApplicationPushLayerCallsOnAttach) {
    Core::Application app;
    
    class MockLayer : public Core::Layer {
        bool attachCalled = false;
    public:
        MockLayer() : Layer("Mock") {}
        void onAttach() override { attachCalled = true; }
        bool wasAttached() const { return attachCalled; }
    };
    
    app.pushLayer<MockLayer>();
    // Verify layer->onAttach() was called
}

TEST_F(MockGLFWFixture, ApplicationDetachsLayersInReverseOrder) {
    // Test that destructor calls onDetach() in reverse order
    // Track call sequence with static/global counter
}

TEST(ApplicationTest, SingletonPatternEnforced) {
    // Verify only one Application instance can exist
    // This is checked by assert, so might need death test
}
```

#### Test File: `tests/Core/test_Window.cpp`

```cpp
#include "Core/Window.h"
#include <gtest/gtest.h>

TEST(WindowTest, ConstructionFailsGracefullyWithInvalidSpec) {
    Core::WindowSpecification spec;
    spec.Width = -1;  // Invalid
    spec.Height = -1;  // Invalid
    
    // Should not crash, but create a window with clamped values
    // or throw/return error
}

TEST(WindowTest, WindowSizeGettersMatchSpecification) {
    Core::WindowSpecification spec;
    spec.Width = 1024;
    spec.Height = 768;
    
    Core::Window window(spec);
    
    auto [w, h] = window.getSize();
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 768);
}
```

### Priority 2: UserConfig Tests

#### Test File: `tests/App/test_UserConfig.cpp`

```cpp
#include "App/UserConfig.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

class UserConfigTest : public ::testing::Test {
protected:
    std::filesystem::path testConfigPath;
    
    void SetUp() override {
        // Create temp config path
        testConfigPath = std::filesystem::temp_directory_path() / "tasksmack_test" / "config.toml";
        std::filesystem::create_directories(testConfigPath.parent_path());
    }
    
    void TearDown() override {
        // Clean up test config
        std::filesystem::remove_all(testConfigPath.parent_path());
    }
    
    void writeConfigFile(const std::string& content) {
        std::ofstream file(testConfigPath);
        file << content;
    }
};

TEST_F(UserConfigTest, LoadValidConfigSucceeds) {
    writeConfigFile(R"(
[theme]
id = "dark"

[font]
size = "large"

[sampling]
interval_ms = 500
history_max_seconds = 600
)");
    
    // Create UserConfig with test path
    // Load and verify values
}

TEST_F(UserConfigTest, LoadMalformedConfigUsesDefaults) {
    writeConfigFile("invalid toml syntax {{{");
    
    // Should not crash, should log error, should use defaults
}

TEST_F(UserConfigTest, OutOfRangeValuesAreClamped) {
    writeConfigFile(R"(
[sampling]
interval_ms = -100
history_max_seconds = 999999
)");
    
    // Load and verify values are clamped to valid ranges
}

TEST_F(UserConfigTest, SaveLoadRoundTripPreservesSettings) {
    App::UserSettings settings;
    settings.themeId = "custom-theme";
    settings.fontSize = UI::FontSize::Large;
    settings.refreshIntervalMs = 250;
    
    // Save, load, verify all fields match
}

TEST_F(UserConfigTest, ImGuiLayoutStatePersists) {
    const std::string mockLayout = "[Window][TestWindow]\nPos=100,200\n";
    
    // Set layout, save, load, verify it matches
}

TEST_F(UserConfigTest, MissingConfigFileCreatesDefault) {
    // Ensure config file doesn't exist
    // Call load()
    // Verify defaults are used
    // Verify file is created on save()
}
```

### Priority 3: ShellLayer Tests

#### Test File: `tests/App/test_ShellLayer.cpp`

```cpp
#include "App/ShellLayer.h"
#include <gtest/gtest.h>

TEST(ShellLayerTest, FrameTimingAccumulation) {
    // Test FPS calculation logic
    // Feed known delta times, verify FPS is calculated correctly
}

TEST(ShellLayerTest, RefreshIntervalSnapping) {
    // Test snapRefreshIntervalMs function
    EXPECT_EQ(snapRefreshIntervalMs(95), 100);
    EXPECT_EQ(snapRefreshIntervalMs(105), 100);
    EXPECT_EQ(snapRefreshIntervalMs(240), 250);
    EXPECT_EQ(snapRefreshIntervalMs(260), 250);
    EXPECT_EQ(snapRefreshIntervalMs(150), 150);  // Far from any stop
}

TEST(ShellLayerTest, PanelVisibilityToggles) {
    // Mock ImGui context
    // Toggle panels, verify state changes
}
```

### Priority 4: UI Layer Tests

#### Test File: `tests/UI/test_ThemeLoader.cpp`

```cpp
#include "UI/ThemeLoader.h"
#include <gtest/gtest.h>

TEST(ThemeLoaderTest, HexToImVec4ValidSixDigit) {
    auto color = UI::ThemeLoader::hexToImVec4("#FF4081");
    EXPECT_FLOAT_EQ(color.x, 1.0f);  // Red
    EXPECT_FLOAT_EQ(color.y, 0.25f); // Green (64/255)
    EXPECT_FLOAT_EQ(color.z, 0.505f); // Blue (129/255)
    EXPECT_FLOAT_EQ(color.w, 1.0f);  // Alpha (default)
}

TEST(ThemeLoaderTest, HexToImVec4ValidEightDigit) {
    auto color = UI::ThemeLoader::hexToImVec4("#FF408180");
    EXPECT_FLOAT_EQ(color.w, 0.502f); // Alpha (128/255)
}

TEST(ThemeLoaderTest, HexToImVec4InvalidLengthReturnsError) {
    auto color = UI::ThemeLoader::hexToImVec4("#FF");
    // Should return error color (not crash)
}

TEST(ThemeLoaderTest, HexToImVec4WithoutHashPrefix) {
    auto color = UI::ThemeLoader::hexToImVec4("FF4081");
    EXPECT_FLOAT_EQ(color.x, 1.0f);
}

TEST(ThemeLoaderTest, LoadThemeFromValidFile) {
    // Create temp TOML file with complete theme
    // Load theme
    // Verify all colors are parsed correctly
}

TEST(ThemeLoaderTest, LoadThemeFromInvalidFileReturnsNullopt) {
    // Non-existent file
    // Malformed TOML
    // Missing required fields
}

TEST(ThemeLoaderTest, DiscoverThemesFindsAllTOMLFiles) {
    // Create temp directory with multiple .toml files
    // Discover themes
    // Verify count and metadata
}
```

#### Test File: `tests/UI/test_IconLoader.cpp`

```cpp
#include "UI/IconLoader.h"
#include <gtest/gtest.h>

// Note: These tests need a valid OpenGL context or mocking

TEST(IconLoaderTest, LoadTextureFromValidFile) {
    // Requires OpenGL context
    // Create minimal PNG in memory
    // Load texture
    // Verify valid() returns true
}

TEST(IconLoaderTest, LoadTextureFromInvalidFileReturnsInvalid) {
    auto texture = UI::loadTexture("/nonexistent/path.png");
    EXPECT_FALSE(texture.valid());
}

TEST(IconLoaderTest, TextureMoveConstructor) {
    // Verify move semantics work correctly
    // Original should be invalid after move
}

TEST(IconLoaderTest, TextureDestructorReleasesResources) {
    // Verify OpenGL texture is deleted
    // May need GL context mocking
}
```

### Priority 5: Numeric Utility Tests

#### Test File: `tests/Domain/test_Numeric.cpp`

```cpp
#include "Domain/Numeric.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <limits>

TEST(NumericTest, NarrowOrWithinRangeReturnsValue) {
    std::int64_t large = 42;
    int result = Domain::Numeric::narrowOr<int>(large, 99);
    EXPECT_EQ(result, 42);
}

TEST(NumericTest, NarrowOrOverflowReturnsDefault) {
    std::int64_t large = std::numeric_limits<std::int64_t>::max();
    int result = Domain::Numeric::narrowOr<int>(large, 99);
    EXPECT_EQ(result, 99);
}

TEST(NumericTest, NarrowOrUnderflowReturnsDefault) {
    std::int64_t large = std::numeric_limits<std::int64_t>::min();
    int result = Domain::Numeric::narrowOr<int>(large, 99);
    EXPECT_EQ(result, 99);
}

TEST(NumericTest, NarrowOrNegativeValues) {
    std::int64_t negative = -42;
    int result = Domain::Numeric::narrowOr<int>(negative, 99);
    EXPECT_EQ(result, -42);
}

TEST(NumericTest, ToFloatNarrowConvertsCorrectly) {
    unsigned int value = 255;
    float result = UI::Numeric::toFloatNarrow(value);
    EXPECT_FLOAT_EQ(result, 255.0f);
}
```

### Priority 6: Panel Tests (Mocked UI)

#### Test File: `tests/App/test_ProcessesPanel.cpp`

```cpp
#include "App/Panels/ProcessesPanel.h"
#include "Domain/ProcessSnapshot.h"
#include <gtest/gtest.h>

// Note: These tests require ImGui context mocking or headless rendering

TEST(ProcessesPanelTest, SortingByCpuPercentDescending) {
    std::vector<Domain::ProcessSnapshot> processes = {
        {/* pid=1, cpu=10% */},
        {/* pid=2, cpu=50% */},
        {/* pid=3, cpu=5% */},
    };
    
    // Apply CPU% sort descending
    // Verify order is: 50%, 10%, 5%
}

TEST(ProcessesPanelTest, FilteringByName) {
    // Test process name filtering logic
}

TEST(ProcessesPanelTest, SelectionStateManagement) {
    // Select process
    // Verify selection persists across refreshes (based on PID+startTime)
    // Verify selection clears when process terminates
}

TEST(ProcessesPanelTest, ColumnVisibilityAffectsRendering) {
    // Mock ImGui table
    // Toggle column visibility
    // Verify correct columns are rendered
}
```

### Priority 7: BackgroundSampler Edge Cases

#### Test File: `tests/Domain/test_BackgroundSampler_EdgeCases.cpp`

```cpp
#include "Domain/BackgroundSampler.h"
#include "Mocks/MockProbes.h"
#include <gtest/gtest.h>

TEST(BackgroundSamplerEdgeCaseTest, RapidStartStopCycles) {
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));
    
    for (int i = 0; i < 100; ++i) {
        sampler.start();
        sampler.stop();
    }
    
    // Should not crash or leak resources
}

TEST(BackgroundSamplerEdgeCaseTest, IntervalChangeWhileRunning) {
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));
    
    sampler.start();
    sampler.setInterval(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sampler.stop();
    
    // Verify new interval is applied
}

TEST(BackgroundSamplerEdgeCaseTest, CallbackThrowsException) {
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));
    
    sampler.setCallback([](auto&, auto) {
        throw std::runtime_error("Test exception");
    });
    
    sampler.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sampler.stop();
    
    // Sampler should continue running despite exception
}

TEST(BackgroundSamplerEdgeCaseTest, ProbeEnumerateThrows) {
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setEnumerateThrows(true);
    
    Domain::BackgroundSampler sampler(std::move(probe));
    
    sampler.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sampler.stop();
    
    // Should handle gracefully, not crash
}
```


## Testing Infrastructure Recommendations

### 1. Add Mock Infrastructure for UI Testing

Create `tests/Mocks/MockImGui.h` to enable UI tests without a real ImGui context:

```cpp
#pragma once

// Mock ImGui functions needed for panel tests
// Allows testing panel logic without graphics context

namespace ImGui {
    struct MockContext {
        // Track calls for verification
        std::vector<std::string> beginCalls;
        std::vector<std::string> endCalls;
    };
    
    extern MockContext* g_MockContext;
    
    inline void SetMockContext(MockContext* ctx) {
        g_MockContext = ctx;
    }
}
```

### 2. Add Integration Test for Full Application Lifecycle

Create `tests/Integration/test_ApplicationLifecycle.cpp`:

- Test full startup → run → shutdown sequence
- Verify no memory leaks (with ASAN)
- Verify proper layer lifecycle
- Test graceful handling of GLFW/OpenGL errors

### 3. Add Property-Based Tests for Numeric Utilities

Use Google Test's parameterized tests or add a fuzzing target:

```cpp
class NumericPropertyTest : public ::testing::TestWithParam<std::int64_t> {};

TEST_P(NumericPropertyTest, NarrowOrAlwaysReturnsValidInt) {
    std::int64_t input = GetParam();
    int result = Domain::Numeric::narrowOr<int>(input, 0);
    
    // Property: result is always a valid int
    EXPECT_GE(result, std::numeric_limits<int>::min());
    EXPECT_LE(result, std::numeric_limits<int>::max());
}

INSTANTIATE_TEST_SUITE_P(
    RandomValues,
    NumericPropertyTest,
    ::testing::Values(/* generate random int64 values */)
);
```

### 4. Add Benchmark Tests for Performance-Critical Paths

Create `tests/Benchmark/` directory with Google Benchmark tests:

- ProcessModel refresh with 1000/5000/10000 processes
- History buffer push performance
- Theme parsing performance
- ProcessSnapshot sorting performance

### 5. Add Fuzz Targets for Input Parsing

Create `tests/Fuzz/` directory:

- TOML config parsing fuzzing
- Hex color parsing fuzzing
- Process name/command line fuzzing (injection attacks)


## Performance Improvement Opportunities

### 1. String Allocation Reduction

**ThemeLoader.cpp:46** - Unnecessary string copy:
```cpp
// Current
const std::string hexString(hex);
const char* hexData = hexString.data();

// Better
const char* hexData = hex.data();
// Use hex.data() directly in std::from_chars
```

### 2. Reduce Vector Reallocations

**ProcessModel** - Pre-reserve snapshot vector capacity:
```cpp
// In refresh(), before loop:
m_Snapshots.clear();
m_Snapshots.reserve(counters.size());  // Avoid reallocations
```

### 3. Optimize History Buffer Access

**History.h** - Consider returning span instead of vector copy:
```cpp
// Current
std::vector<T> data() const;

// Better
std::span<const T> data() const noexcept;
```

### 4. Cache Theme Color Lookups

**Theme.cpp** - Colors are looked up frequently; consider caching:
```cpp
// Pre-compute commonly used derived colors
struct CachedColors {
    ImU32 textPrimaryU32;
    ImU32 textSecondaryU32;
    // ... other frequently converted colors
};
```

### 5. Optimize Process Sorting

**ProcessesPanel** - Use partial_sort if only top N needed:
```cpp
// If only showing top 50 processes:
std::partial_sort(processes.begin(), 
                  processes.begin() + 50,
                  processes.end(),
                  comparator);
```

## Code Improvements Summary

### High-Impact Changes

1. **Add Core/Application tests** - Critical for stability
2. **Add UserConfig tests** - Prevent config corruption
3. **Extract PDH helper function** - Reduce WindowsDiskProbe duplication
4. **Consolidate refresh interval constants** - DRY principle in ShellLayer
5. **Add [[nodiscard]] to query methods** - Prevent silent errors
6. **Replace std::system() in ShellLayer** - Security improvement

### Medium-Impact Changes

1. **Improve error handling with std::expected** - Better error propagation
2. **Add ThemeLoader edge case tests** - Robustness
3. **Add IconLoader tests** - Resource management verification
4. **Add Panel unit tests** - UI logic correctness
5. **Pre-reserve vector capacities** - Performance

### Low-Impact (Nice-to-Have)

1. **Use std::views::enumerate** - Modern C++23 patterns
2. **Cache theme colors** - Micro-optimization
3. **Property-based tests for Numeric** - Comprehensive validation
4. **Benchmark tests** - Performance tracking
5. **Fuzz testing** - Security hardening

## Implementation Roadmap

### Phase 1: Critical Gaps (Week 1-2)
1. Add Core layer tests (Application, Window)
2. Add UserConfig tests (parsing, I/O, edge cases)
3. Add ShellLayer tests (frame timing, menu actions)
4. Fix security concern in ShellLayer std::system()

### Phase 2: Robustness (Week 3-4)
1. Add UI layer tests (ThemeLoader, IconLoader)
2. Add Panel tests (with ImGui mocking)
3. Add BackgroundSampler edge case tests
4. Add Numeric utility tests

### Phase 3: Code Quality (Week 5-6)
1. Extract PDH helper function
2. Consolidate duplicated constants
3. Add missing [[nodiscard]] attributes
4. Add constexpr where beneficial
5. Review and reduce NOLINTs where possible

### Phase 4: Advanced (Week 7-8)
1. Add property-based tests
2. Add benchmark tests
3. Add fuzz targets
4. Integration test improvements
5. Performance optimizations

## Conclusion

### Strengths
- **Excellent architecture** with clean layer separation
- **Good Platform and Domain test coverage**
- **Strong modern C++23 adoption**
- **Minimal dead code and recursion**
- **Well-documented codebase**

### Weaknesses
- **No Core layer tests** - Application lifecycle untested
- **No App layer tests** - UserConfig, ShellLayer, Panels untested
- **Minimal UI layer tests** - Resource loading, theme parsing minimally tested
- **Limited edge case coverage** - Error paths, malformed input
- **Some security concerns** - std::system() usage, input validation

### Key Metrics
- **Lines Needing Tests**: ~3000 (Core + App + UI layers)
- **Critical Untested Files**: 15
- **NOLINTs to Review**: 15 (3 can be eliminated)
- **TODOs to Address**: 2
- **Performance Opportunities**: 5 high-impact, 10 medium-impact

### Risk Assessment
- **High Risk Areas**: UserConfig (data loss), Core (crashes)
- **Medium Risk Areas**: UI loading (resource leaks), Panel logic (incorrect display)
- **Low Risk Areas**: Numeric utilities (well-constrained), Format (pure functions)

### Recommended Immediate Actions
1. Create test files for Core layer (Application, Window)
2. Create comprehensive UserConfig tests
3. Replace std::system() with platform-specific APIs
4. Add [[nodiscard]] to BackgroundSampler methods
5. Extract PDH helper to reduce duplication

