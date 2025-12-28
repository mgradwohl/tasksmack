# GitHub Issues to Create from Code Review

This document lists issues identified during the systematic code review that should be created as GitHub issues for tracking.

## Feature Requests

### Issue 1: Options Dialog UI
**Title**: Add global Options/Preferences dialog

**Labels**: `enhancement`, `ui`, `good-first-issue`

**Description**:
Add a comprehensive Options dialog accessible from the File menu to configure global application settings.

**Features to include**:
- Theme selection (dark/light/custom)
- Default refresh interval
- Column visibility presets
- Font size selection
- Startup behavior (restore window position/size)
- Export/import configuration

**Related code**: `src/App/ShellLayer.cpp:467`

**Priority**: Medium

---

### Issue 2: Windows per-process network counters
**Title**: Implement per-process network monitoring on Windows

**Labels**: `enhancement`, `windows`, `platform`

**Description**:
Currently, Windows probe does not support per-process network counters (network sent/received bytes). Implement this using either:

1. **ETW (Event Tracing for Windows)** - Recommended
   - Subscribe to kernel providers for network events
   - Associate events with processes by PID
   - Aggregate sent/received bytes

2. **GetPerTcpConnectionEStats** API
   - Enumerate TCP connections per process
   - Query statistics for each connection
   - Sum across connections

**Implementation notes**:
- ETW provides more comprehensive coverage (includes UDP, other protocols)
- Requires elevated privileges for kernel-level tracing
- Consider performance impact of high-frequency sampling

**Related code**: `src/Platform/Windows/WindowsProcessProbe.cpp:397`

**Related documentation**: [tasksmack.md](tasksmack.md) "Per-Process Network and I/O Monitoring" section

**Priority**: Low (nice-to-have feature)

---

## Performance Improvements

### Issue 3: Optimize snapshot copying with shared_ptr
**Title**: Reduce memory allocations in snapshot access pattern

**Labels**: `enhancement`, `performance`, `domain`

**Description**:
Currently, `ProcessModel::snapshots()` and `SystemModel::snapshot()` copy the entire snapshot on every access for thread safety. For systems with 1000+ processes, this copies ~200KB per call.

**Proposal**: Use `std::shared_ptr<const std::vector<ProcessSnapshot>>` with atomic operations:

```cpp
// In ProcessModel.h
std::shared_ptr<const std::vector<ProcessSnapshot>> m_Snapshots;

// Update (in mutex-protected section)
auto newSnaps = std::make_shared<const std::vector<ProcessSnapshot>>(std::move(computed));
std::atomic_store(&m_Snapshots, newSnaps);

// Read (lock-free)
return std::atomic_load(&m_Snapshots);
```

**Benefits**:
- Eliminates copies (reference counting instead)
- Lock-free reads (atomic ptr load)
- Still thread-safe
- Reduces allocation pressure

**Considerations**:
- Benchmark before/after to quantify improvement
- Ensure atomic operations are correctly ordered
- Test with TSan (thread sanitizer)

**Files affected**:
- `src/Domain/ProcessModel.h`
- `src/Domain/ProcessModel.cpp`
- `src/Domain/SystemModel.h`
- `src/Domain/SystemModel.cpp`

**Priority**: Medium (measurable performance improvement)

---

## Testing

### Issue 4: Add UserConfig unit tests
**Title**: Add unit tests for UserConfig in App layer

**Labels**: `testing`, `enhancement`, `good-first-issue`

**Description**:
The App layer needs comprehensive unit tests for UserConfig. Note: test_ProcessColumnConfig.cpp is being completed in PR #281 (this PR).

**Tests to add**:

1. **test_UserConfig.cpp** (NEW - priority)
   - Config loading from TOML file
   - Default value handling
   - Invalid config recovery
   - Config saving
   - Platform-specific paths (XDG_CONFIG_HOME, APPDATA)

**Status**:
- ✅ test_ProcessColumnConfig.cpp - Completed in this PR
  - 10 comprehensive tests covering column operations
  - Column index conversions, visibility toggling, boundary conditions

**Setup required**:
- Use temporary directories for config file tests
- Mock filesystem operations where needed
- Test both Linux and Windows code paths (conditional compilation)

**Files to create**:
- `tests/App/test_UserConfig.cpp`

**Priority**: Medium (improves maintainability)

---

### Issue 5: Add integration tests for threading scenarios
**Title**: Test BackgroundSampler concurrent access patterns

**Labels**: `testing`, `threading`, `integration`

**Description**:
Add integration tests to verify thread-safe behavior of BackgroundSampler with ProcessModel/SystemModel.

**Test scenarios**:
1. Concurrent reads while sampler is updating
2. Stopping sampler while read in progress
3. Changing sampling interval during operation
4. Callback execution ordering
5. Memory barriers and synchronization

**Tools**:
- Run with ThreadSanitizer (TSan) to detect races
- Stress test with high sampling rates
- Verify no data races under load

**File to create**: `tests/Integration/test_BackgroundSamplerThreading.cpp`

**Priority**: Low (current code appears correct, this adds verification)

---

## Code Quality

### Issue 6: Audit for std::string_view opportunities
**Title**: Replace `const std::string&` parameters with `std::string_view` where appropriate

**Labels**: `enhancement`, `modernization`, `good-first-issue`

**Description**:
Many functions take `const std::string&` parameters for read-only access. `std::string_view` is more flexible (works with `char*`, string literals, substrings) and avoids hidden allocations.

**Candidates** (from grep search):
- Functions in Format.h that take strings for formatting
- ProcessSnapshot/SystemSnapshot comparisons
- Logging helper functions

**Important**: Do NOT change parameters where:
- String is stored (needs ownership)
- String is moved (move semantics required)
- String is used in a std::map key
- Lifetime is unclear

**Example transformation**:
```cpp
// Before
void logError(const std::string& message);

// After
void logError(std::string_view message);
```

**Priority**: Low (micro-optimization, improves API)

---

## Documentation

### Issue 7: Document NOLINT suppressions
**Title**: Add explanatory comments to all NOLINT suppressions

**Status**: ✅ **COMPLETED** in this PR

**Description**:
Two NOLINT suppressions (`std::getenv`, `std::system`) were lacking explanatory comments. These have been documented with:
- Why the suppression is necessary
- Why the usage is safe in this context
- What alternatives were considered

**Files updated**:
- `src/App/UserConfig.cpp:63` - Documented std::getenv usage
- `src/App/ShellLayer.cpp:129` - Documented std::system usage

---

## Summary

Total issues identified: **7**
- Completed: 1 (NOLINT documentation)
- To create: 6 new GitHub issues
  - **Feature requests**: 2 (Options dialog, Windows network counters)
  - **Performance**: 1 (shared_ptr optimization)
  - **Testing**: 2 (App tests, threading tests)
  - **Code quality**: 1 (string_view audit)

All issues include:
- Clear description
- Suggested implementation approach
- Related files/code locations
- Priority assessment
- Appropriate labels

---

## Priority Tiers

**High**: None (all identified issues are enhancements)

**Medium**:
- Issue 1: Options Dialog
- Issue 3: Snapshot copying optimization
- Issue 4: App layer tests

**Low**:
- Issue 2: Windows network counters
- Issue 5: Threading tests
- Issue 6: string_view audit
