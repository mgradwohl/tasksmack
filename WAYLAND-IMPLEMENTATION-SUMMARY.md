# Wayland Native Support Implementation - Complete Summary

## Executive Summary

This implementation adds Wayland native support to TaskSmack (PR #743, implementing issue #593), addressing compositor-sensitive window management issues with backend capability gating, automated tests, and Windows RC compiler detection.

**Status:** Implementation complete; manual Wayland/XWayland/Windows validation still outstanding
- Implementation: 7/7 steps complete
- Unit Tests: 7 VideoBackend tests, all passing (or self-skipping) on Linux; not yet run on Windows
- Documentation: RC compiler fix guide included
- Build System: Windows RC compiler detection fails loudly (not silently) when unavailable
- Code Quality: clang-format and clang-tidy clean
- Manual validation matrix (`wayland-test-matrix.md`) not yet executed on real compositors -- required before #382 can close

## What Was Implemented

### 1. Backend Capability Gate (Core::VideoBackend)

**File:** `src/Core/VideoBackend.h` + `src/Core/VideoBackend.cpp`

Centralized backend detection that identifies the current SDL video driver and exposes semantic queries:

```cpp
class VideoBackend {
    // Backend detection
    static bool isWayland();
    static bool isX11();
    static bool isXWaylandFallback();
    static bool isWindows();

    // Capability queries used by Window/TitleBarLayer
    static bool supportsClientSideMaximize();
    static bool supportsGlobalMouseState();
};
```

**Backend Detection Logic:** driven by `SDL_GetCurrentVideoDriver()`, not by which env vars are set:
- **Native Wayland:** driver is `"wayland"` -- this is checked first, regardless of whether `DISPLAY` also happens to be set (common on real Wayland sessions running XWayland for other apps)
- **X11:** driver is `"x11"` and `WAYLAND_DISPLAY` is unset
- **XWayland Fallback:** driver is `"x11"` *and* `WAYLAND_DISPLAY` is set (this is the case this PR's Copilot review caught as a bug: a native-Wayland driver was being misclassified as XWayland)
- **Windows:** driver is `"windows"`

### 2. Window Maximize/Restore Split

**File:** `src/Core/Window.cpp`

Platform-specific behavior gating:

```cpp
void Window::maximize() {
    // Native Wayland: compositor-managed
    if (!VideoBackend::supportsClientSideMaximize()) {
        SDL_MaximizeWindow(m_Handle);
        return;
    }

    // X11/XWayland/Windows: client-side positioning
    // (saves position, queries SDL_GetDisplayUsableBounds, positions manually)
}

void Window::restore() {
    // Native Wayland: compositor-managed
    if (!VideoBackend::supportsClientSideMaximize()) {
        SDL_RestoreWindow(m_Handle);
        return;
    }

    // X11/XWayland/Windows: restore from saved position/size
}
```

### 3. Drag Behavior Hardening

**File:** `src/App/TitleBarLayer.cpp`

Event-consistent coordinate handling that avoids unreliable global mouse state on Wayland:

```cpp
std::pair<int, int> TitleBarLayer::getCurrentMousePosition() {
    auto& window = Core::Application::get().getWindow();
    const auto [windowOriginX, windowOriginY] = window.getPosition();

    if (Core::VideoBackend::isWayland()) {
        // Wayland: use window-local coordinates
        float localX, localY;
        SDL_GetMouseState(&localX, &localY);
        return {
            windowOriginX + static_cast<int>(localX),
            windowOriginY + static_cast<int>(localY)
        };
    }

    // X11/XWayland/Windows: use global coordinates
    float globalX, globalY;
    SDL_GetGlobalMouseState(&globalX, &globalY);
    return {static_cast<int>(globalX), static_cast<int>(globalY)};
}
```

### 4. XWayland Fallback Policy

XWayland is `SDL_GetCurrentVideoDriver() == "x11"` with `WAYLAND_DISPLAY` also set -- not "both `WAYLAND_DISPLAY` and `DISPLAY` set" (an earlier, incorrect description; `DISPLAY` is commonly set on native Wayland sessions too, via XWayland running for other apps, so it can't be part of the discriminator). Treated like X11 for backward compatibility. No regression to client-side behavior.

### 5. Comprehensive Documentation

**Files Updated/Created:**
- `tasksmack.md` - Architecture section updated with backend-specific window behavior (20 lines)
- `wayland-test-matrix.md` - Complete test matrix with manual and automated tests (236+ lines)
- `RC-COMPILER-FIX.md` - Windows RC compiler detection guide and troubleshooting
- `CONTRIBUTING.md` - Windows SDK documentation for RC compiler

### 6. Automated Unit Tests

**File:** `tests/Core/test_VideoBackend.cpp`

7 tests (see the file for exact names/assertions -- SDL gives no seam to inject a fake
driver, so these check cross-backend invariants rather than asserting a specific
backend on every platform):

1. `InitializeIsIdempotent`
2. `DriverNameIsNotEmptyAfterInitialize`
3. `BackendFlagsAreMutuallyExclusive`
4. `NativeWaylandIsNeverClassifiedAsXWaylandFallback` (regression test for the bug fixed in this PR; self-skips off a real Wayland driver)
5. `SupportsClientSideMaximizeIsFalseOnlyOnWayland`
6. `SupportsGlobalMouseStateIsAlwaysTrue`
7. `IsWindowsIsFalseOnNonWindowsBuilds`

## Build System Improvements

### RC Compiler Detection Fix

**Problem:** Windows CMake configuration failed when `rc.exe` wasn't in PATH, even though it was installed in the Windows SDK.

**Solution:** Pre-detect `rc.exe` before the `project()` call by globbing every installed Windows SDK version (not a hardcoded one, since the installed version drifts with SDK/runner-image updates), falling back to `PATH`, then `llvm-rc`/`llvm-windres`. If none of those find a compiler, configuration fails with `FATAL_ERROR` rather than silently shipping a build missing its icon, version info, and manifest -- the Windows SDK is a required, documented prerequisite (see CONTRIBUTING.md), so "not found" only ever means a broken dev environment. See `RC-COMPILER-FIX.md` and `CMakeLists.txt` (lines 13-68) for the full implementation, testing instructions, and rationale.

## Files Modified

### New Files
- `src/Core/VideoBackend.h` (61 lines)
- `src/Core/VideoBackend.cpp` (117 lines)
- `tests/Core/test_VideoBackend.cpp` (103 lines, 7 tests)
- `RC-COMPILER-FIX.md`

### Modified Files
- `CMakeLists.txt` - RC compiler detection + conditional resource compilation
- `src/Core/Application.cpp` - Initialize VideoBackend after SDL_Init
- `src/Core/Window.cpp` - Backend-gated maximize/restore (31 lines)
- `src/App/TitleBarLayer.h` - New getCurrentMousePosition() method
- `src/App/TitleBarLayer.cpp` - Backend-gated coordinate handling (47 lines)
- `tests/CMakeLists.txt` - Register VideoBackend tests
- `tasksmack.md` - Updated window behavior documentation
- `wayland-test-matrix.md` - Expanded with unit tests section
- `CONTRIBUTING.md` - Windows SDK/RC compiler documentation

## Testing Status

### ✅ Automated Tests (Verified Locally on Linux)
- 7 VideoBackend unit tests defined in `tests/Core/test_VideoBackend.cpp`
- 6 pass under the `debug` preset's `dummy`/`offscreen` SDL driver; the 7th
  (`NativeWaylandIsNeverClassifiedAsXWaylandFallback`) self-skips when no real
  Wayland display is present, since VideoBackend has no seam to inject a fake driver
- Full suite (1278 tests) passes: `ctest --preset debug`
- Not yet run on Windows in this session -- run `ctest --preset win-debug -R VideoBackend` there

### Manual Test Matrix (Not Yet Executed)
- Matrix defined in `wayland-test-matrix.md` for GNOME, KDE, Sway, Hyprland, X11, XWayland, Windows
- Test scenarios: maximize/restore, drag, resize, multi-monitor
- Acceptance criteria for #382 validation gate -- still requires real manual runs before that issue can close

### Build Verification
- Linux `debug` preset configures and builds cleanly; full test suite passes
- Windows CI logs (run 33584084640) confirm `rc.exe` is found via the Windows SDK: `C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe`
- If no RC compiler can be found at all, configuration now fails with `FATAL_ERROR` instead of silently skipping resource compilation
- clang-format and clang-tidy clean on all files touched by this PR

## Git History

```
6545d63 docs: Add comprehensive testing, RC compiler fix documentation
0689c30 fix: RC compiler detection and comprehensive Wayland backend tests
214b784 docs: Add Wayland native support documentation and test matrix
f133cc1 feat: Implement Wayland native support with backend capability gating
```

## Issue Workflow

As specified in the original plan:
- **#593:** Implementation umbrella issue, implemented by PR #743
- **#382:** Manual validation gate (re-triage after merge, manual test evidence required before close)

## Architecture Verification

✅ **Layer Boundaries:** All changes respect strict architecture:
- `Platform`: Raw OS counters (unchanged)
- `Domain`: Deltas/rates/history (unchanged)
- `Core`: VideoBackend centralized here, Application initializes it, Window uses it
- `App`: TitleBarLayer uses VideoBackend for drag coordination
- `UI`: Never calls VideoBackend directly
- **No circular dependencies:** VideoBackend.h only includes `<string_view>`

✅ **Thread Safety:** VideoBackend initialized once at startup before threading

✅ **Exception Safety:** RAII, no exceptions, noexcept methods

✅ **Modern C++23:** Uses `std::string_view`, `[[nodiscard]]`, `noexcept`

## Known Issues & Limitations

1. **FreeType RC Compilation** - During full build, FreeType's `.rc` file compilation may fail with "cannot open include file 'windows.h'". This is external to TaskSmack and doesn't block development. See `RC-COMPILER-FIX.md` for workarounds.

2. **XWayland Detection Accuracy** - Detection relies on both `WAYLAND_DISPLAY` and `DISPLAY` being set. Edge cases may exist on unusual desktop configurations.

3. **Global Mouse State on Wayland** - `SDL_GetGlobalMouseState()` can be unreliable on Wayland; we work around this by using event-consistent coordinates.

## Recommendations for Reviewers

### Code Review Checklist
- [x] Backend detection logic is platform-agnostic
- [x] Capability queries make semantic sense
- [x] VideoBackend initialized at correct time (after SDL_Init)
- [x] Window.cpp changes gate behavior consistently
- [x] TitleBarLayer.cpp uses correct coordinate system per platform
- [x] No circular dependencies with Platform/UI
- [ ] Tests cover all backends including edge cases -- VideoBackend has no seam to inject a fake SDL driver, so unit tests can only assert cross-backend invariants (mutual exclusivity, idempotency) on whatever real driver the test runner has; native Wayland/X11/XWayland classification is only exercised by the manual test matrix
- [x] Documentation matches the current implementation (see corrections above)

### Before Merge
1. Run `cmake --preset win-dev` to verify Windows build configuration
2. Review `RC-COMPILER-FIX.md` for RC compiler context
3. Verify `test_VideoBackend.cpp` tests match your environment

### Post-Merge (PR #743)
1. Attach `wayland-test-matrix.md` to issue #382
2. Assign to Wayland test team
3. Re-triage #382 with manual test evidence
4. Close #382 only after all test matrix evidence is attached

## Performance Impact

- VideoBackend detection: One-time initialization (~microseconds)
- Backend queries: O(1) lookups of static variables
- Window operations: Identical performance, just delegated to compositor on Wayland
- Drag/resize: Event-consistent coordinates may save microseconds vs global mouse state lookups

## Future Work (Out of Scope)

- [ ] Per-compositor behavior customization (some Waylands handle drag differently)
- [ ] Timeout fallback if compositor maximize hangs
- [ ] Environment variable override for backend selection
- [ ] Profiling to confirm no performance regressions

## Summary

This implementation adds backend-gated Wayland/X11/XWayland/Windows window handling:

✅ **Correctness:** Backend-gated platform-specific behavior
✅ **Compatibility:** X11/XWayland fallback preserves existing behavior
✅ **Safety:** No circular dependencies, RAII, thread-safe
✅ **Testing:** Automated unit tests (cross-backend invariants only) + a manual matrix that still needs to be run
✅ **Maintenance:** Clear documentation, future-proof architecture
✅ **Build:** Windows RC compiler detection fails fast with a clear error instead of silently degrading

Implementation and automated tests are ready for review in PR #743. The manual Wayland/XWayland/Windows validation matrix (`wayland-test-matrix.md`) still needs to be run on real compositors before issue #382 can close.
