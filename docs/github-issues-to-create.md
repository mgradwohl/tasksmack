# GitHub Issues to Create from Code Review Analysis

This document contains 16 grouped issues identified in the comprehensive code review. Each issue should be created with the **code-review** label.

---

## HIGH PRIORITY ISSUES

### Issue 1: Add Core Layer Tests (Application and Window)
**Labels**: `code-review`, `testing`, `high-priority`

The Core layer has no test coverage, representing high risk for crashes and initialization failures.

**Files**: `src/Core/Application.cpp`, `src/Core/Window.cpp`

**Tasks**:
- [ ] Add unit tests for Application construction/destruction
- [ ] Add unit tests for layer stack operations (push, iterate, detach in reverse)
- [ ] Add integration tests with mocked GLFW window creation
- [ ] Add edge case tests for GLFW initialization failure
- [ ] Add edge case tests for window creation failure

**Reference**: See `docs/test-coverage-analysis.md` section "Core Layer (Application Lifecycle)" and `docs/test-coverage-implementation-guide.md` Priority 2.

---

### Issue 2: Add UserConfig Tests (TOML Parsing and File I/O)
**Labels**: `code-review`, `testing`, `high-priority`

UserConfig has no test coverage for TOML parsing, file I/O, or platform-specific paths, risking data loss and config corruption.

**Files**: `src/App/UserConfig.cpp`

**Tasks**:
- [ ] Add tests for config path resolution (Linux XDG_CONFIG_HOME, Windows APPDATA)
- [ ] Add tests for valid TOML parsing
- [ ] Add tests for partial TOML (missing fields use defaults)
- [ ] Add tests for invalid TOML (syntax errors)
- [ ] Add tests for out-of-range values (clamping)
- [ ] Add tests for ImGui layout serialization/deserialization
- [ ] Add integration tests for save/load round-trip

**Reference**: See `docs/test-coverage-analysis.md` section "UserConfig" and `docs/test-coverage-implementation-guide.md` Priority 3.

---

### Issue 3: Add ShellLayer Tests (UI Orchestration)
**Labels**: `code-review`, `testing`, `high-priority`

ShellLayer has no test coverage for menu actions, panel coordination, and frame timing.

**Files**: `src/App/ShellLayer.cpp`

**Tasks**:
- [ ] Add tests for frame timing accumulation and FPS calculation
- [ ] Add tests for panel visibility state management
- [ ] Add tests for refresh interval snapping logic
- [ ] Add mock tests for file opening (avoid actual shell exec)

**Reference**: See `docs/test-coverage-analysis.md` section "ShellLayer" and `docs/test-coverage-implementation-guide.md`.

---

### Issue 4: 🔐 SECURITY: Replace std::system() with Platform-Specific APIs
**Labels**: `code-review`, `security`, `high-priority`

**SECURITY VULNERABILITY**: `ShellLayer.cpp:129` uses `std::system()` with user-controlled file paths, creating command injection risk.

**Files**: `src/App/ShellLayer.cpp` (line 129)

**Current vulnerable code**:
```cpp
const std::string command = "xdg-open \"" + filePath.string() + "\" &";
const int result = std::system(command.c_str());
```

**Tasks**:
- [ ] Create `src/Platform/IFileOpener.h` interface
- [ ] Implement Linux version using fork/exec with proper argument handling
- [ ] Improve Windows version (already uses ShellExecuteW for notepad, but fallback is vulnerable)
- [ ] Move file opening logic to Platform layer
- [ ] Add tests for the new implementation

**Reference**: See `docs/test-coverage-analysis.md` "ShellLayer Security Note" and `docs/test-coverage-implementation-guide.md` Priority 1 (30 minutes).

---

## MEDIUM PRIORITY ISSUES

### Issue 5: Add UI Layer Tests (ThemeLoader, IconLoader, UILayer)
**Labels**: `code-review`, `testing`, `medium-priority`

UI layer has minimal test coverage for resource loading and theme parsing.

**Files**: `src/UI/ThemeLoader.cpp`, `src/UI/IconLoader.cpp`, `src/UI/UILayer.cpp`

**Tasks**:
- [ ] Add tests for hex color parsing edge cases (invalid format, 6-digit, 8-digit with alpha)
- [ ] Add tests for theme discovery from multiple directories
- [ ] Add tests for theme loading with missing fields
- [ ] Add tests for malformed TOML theme files
- [ ] Add mock tests for icon loader without real OpenGL context

**Reference**: See `docs/test-coverage-analysis.md` section "UI Layer" and `docs/test-coverage-implementation-guide.md` Priority 5.

---

### Issue 6: Add Panel Tests (Complex UI Logic)
**Labels**: `code-review`, `testing`, `medium-priority`

Panel components have no test coverage for sorting, filtering, and selection logic.

**Files**: `src/App/Panels/ProcessesPanel.cpp`, `ProcessDetailsPanel.cpp`, `SystemMetricsPanel.cpp`, `StoragePanel.cpp`

**Tasks**:
- [ ] Add tests for sorting logic (each column type)
- [ ] Add tests for process selection state management
- [ ] Add tests for column visibility toggles
- [ ] Add integration tests for process actions (with mock IProcessActions)

**Reference**: See `docs/test-coverage-analysis.md` section "Panel Components".

---

### Issue 7: Add BackgroundSampler Edge Case Tests
**Labels**: `code-review`, `testing`, `medium-priority`

BackgroundSampler has basic tests but missing edge case coverage.

**Files**: `src/Domain/BackgroundSampler.cpp`

**Tasks**:
- [ ] Add stress test for rapid start/stop cycles
- [ ] Add test for interval changes during operation
- [ ] Add test for callback throwing exception (sampler should continue)
- [ ] Add test for probe enumerate throwing exception (handle gracefully)

**Reference**: See `docs/test-coverage-analysis.md` section "BackgroundSampler Edge Cases".

---

### Issue 8: Review and Improve Theme.h Member Initialization
**Labels**: `code-review`, `code-quality`, `medium-priority`

Theme.h has a large NOLINT block (110 lines) disabling `readability-redundant-member-init`.

**Files**: `src/UI/Theme.h` (lines 50-160)

**Tasks**:
- [ ] Review if member initializations can use in-class initializers
- [ ] Determine if NOLINT block can be more targeted
- [ ] Refactor if possible to reduce or eliminate NOLINT

**Reference**: See `docs/test-coverage-analysis.md` "NOLINT Usage Audit".

---

## LOW PRIORITY ISSUES (Quick Wins)

### Issue 9: Add [[nodiscard]] Attributes to Query Methods
**Labels**: `code-review`, `code-quality`, `good-first-issue`

Several query methods missing `[[nodiscard]]` could lead to silent bugs.

**Files**:
- `src/Domain/BackgroundSampler.h` (lines ~45, ~60)

**Tasks**:
- [ ] Add `[[nodiscard]]` to `BackgroundSampler::isRunning()`
- [ ] Add `[[nodiscard]]` to `BackgroundSampler::interval()`
- [ ] Review Format.h helper functions

**Estimated time**: 15 minutes

**Reference**: See `docs/test-coverage-analysis.md` "[[nodiscard]] Usage Review".

---

### Issue 10: Consolidate Duplicated Refresh Interval Constants
**Labels**: `code-review`, `code-quality`, `good-first-issue`

Refresh interval stops array is duplicated in ShellLayer.cpp (lines 46 and 75).

**Files**: `src/App/ShellLayer.cpp`

**Task**:
- [ ] Define constant at namespace scope: `constexpr std::array REFRESH_INTERVAL_STOPS = {100, 250, 500, 1000};`
- [ ] Use in both `snapRefreshIntervalMs()` and `drawRefreshPresetTicks()`

**Estimated time**: 10 minutes

**Reference**: See `docs/test-coverage-analysis.md` "Duplication Opportunities".

---

### Issue 11: Extract PDH Helper Function to Reduce Duplication
**Labels**: `code-review`, `code-quality`, `good-first-issue`

WindowsDiskProbe.cpp repeats PDH union access with NOLINT 5 times.

**Files**: `src/Platform/Windows/WindowsDiskProbe.cpp` (lines 206, 212, 219, 225, 233)

**Task**:
- [ ] Create helper function: `[[nodiscard]] inline double extractPdhDouble(const PDH_FMT_COUNTERVALUE& value) noexcept`
- [ ] Replace 5 duplicated instances with helper calls

**Estimated time**: 15 minutes

**Reference**: See `docs/test-coverage-analysis.md` "Duplication Opportunities".

---

### Issue 12: Add Named Constant for Delta Time Clamp
**Labels**: `code-review`, `code-quality`, `good-first-issue`

Application.cpp uses magic number 0.1F for delta time clamping.

**Files**: `src/Core/Application.cpp` (line 89)

**Task**:
- [ ] Add `constexpr float MAX_DELTA_TIME = 0.1F;` at top of file
- [ ] Use in clamp: `deltaTime = std::min(deltaTime, MAX_DELTA_TIME);`

**Estimated time**: 5 minutes

**Reference**: See `docs/test-coverage-analysis.md` "Const Correctness Review".

---

### Issue 13: Add Numeric Utility Tests
**Labels**: `code-review`, `testing`, `low-priority`, `good-first-issue`

Numeric utility functions have no dedicated tests.

**Files**: `src/Domain/Numeric.h`, `src/UI/Numeric.h`

**Tasks**:
- [ ] Add tests for `narrowOr` overflow behavior
- [ ] Add tests for `narrowOr` underflow behavior
- [ ] Add tests for `narrowOr` with negative values
- [ ] Add tests for `toFloatNarrow`

**Estimated time**: 1 hour

**Reference**: See `docs/test-coverage-analysis.md` "Numeric Utilities" and `docs/test-coverage-implementation-guide.md` Quick Win 4.

---

### Issue 14: Document or Implement TODO Items
**Labels**: `code-review`, `documentation`, `low-priority`

Two TODO items should be either implemented or tracked as separate issues.

**TODOs**:
1. `src/App/ShellLayer.cpp:464` - "TODO: Open options dialog"
2. `src/Platform/Windows/WindowsProcessProbe.cpp:397` - "TODO: Implement using ETW or GetPerTcpConnectionEStats"

**Tasks**:
- [ ] Create issue for options dialog feature (or implement if quick)
- [ ] Confirm network counters TODO is already tracked in completed-features.md

**Reference**: See `docs/test-coverage-analysis.md` "TODO/FIXME Items".

---

### Issue 15: Pre-Reserve Vector Capacities in Model Classes
**Labels**: `code-review`, `performance`, `good-first-issue`

Model classes rebuild snapshot vectors without pre-reserving capacity.

**Files**: `src/Domain/ProcessModel.cpp`, `SystemModel.cpp`, `StorageModel.cpp`

**Task**:
- [ ] Add `m_Snapshots.reserve(counters.size());` before loop in each refresh() method

**Estimated time**: 20 minutes

**Reference**: See `docs/test-coverage-analysis.md` "Performance Improvement Opportunities".

---

### Issue 16: Reduce String Allocation in ThemeLoader Hex Parsing
**Labels**: `code-review`, `performance`, `good-first-issue`

ThemeLoader creates unnecessary string copy during hex parsing.

**Files**: `src/UI/ThemeLoader.cpp` (line 46)

**Task**:
- [ ] Use `hex.data()` directly instead of creating `hexString` copy

**Estimated time**: 5 minutes

**Reference**: See `docs/test-coverage-analysis.md` "Performance Improvement Opportunities".

---

## Summary Statistics

- **Total Issues**: 16
- **HIGH Priority**: 4 (3 testing + 1 security)
- **MEDIUM Priority**: 4 (3 testing + 1 code quality)
- **LOW Priority**: 8 (5 good-first-issues)

**Quick Wins** (total ~2 hours):
- Issues 9-12: Code quality improvements (1 hour)
- Issue 13: Numeric tests (1 hour)

**Time Estimates**:
- HIGH priority issues: 1-2 weeks
- MEDIUM priority issues: 1-2 weeks  
- LOW priority issues: 2-3 days

All issues reference the comprehensive analysis at `docs/test-coverage-analysis.md` for implementation details and examples.
