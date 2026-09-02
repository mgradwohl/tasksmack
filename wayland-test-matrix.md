# Wayland Native Support - Test Matrix and Validation

## Overview
This document defines the test matrix and acceptance criteria for the Wayland native support implementation (PR #743, implementing issue #593). All tests should be executed on each platform/compositor combination to validate that #382's manual testing gate is satisfied.

**Issue Workflow:**
- #593: Implementation umbrella issue, implemented by PR #743
- #382: Manual validation gate (re-triage after PR #743 merges, manual test matrix evidence required before close)

## Implementation Summary
The implementation uses backend capability gating (`Core::VideoBackend`) to provide platform-specific behavior:
1. **Native Wayland:** Compositor-managed maximize/restore, event-consistent local-coordinate drag
2. **X11/XWayland:** Client-side maximize/restore, global-coordinate drag (existing behavior preserved)
3. **Windows:** Client-side interactions (existing behavior preserved)

Backend classification is driven by `SDL_GetCurrentVideoDriver()`, not by which env vars happen to be set. Native Wayland is `driver == "wayland"`, checked first -- this matters because `DISPLAY` is commonly set on a Wayland session too (e.g. XWayland running for other apps), so testers should not assume "DISPLAY is set" means an X11/XWayland run. XWayland fallback is `driver == "x11"` *and* `WAYLAND_DISPLAY` set; plain X11 is `driver == "x11"` with `WAYLAND_DISPLAY` unset.

## Test Matrix

### Automated Unit Tests (CI/CD - Required for Merge)

All platform-specific functionality is validated by automated unit tests that run on every build:

#### VideoBackend Detection Tests (tests/Core/test_VideoBackend.cpp)

These tests verify the backend detection and capability query system that gates all platform-specific behavior.

**Test Coverage** (7 `TEST_F` cases; SDL gives no seam to inject a fake driver, so
these check cross-backend invariants rather than asserting a specific backend on
every platform):
- `InitializeIsIdempotent` - calling `initialize()` twice doesn't change the result
- `DriverNameIsNotEmptyAfterInitialize` - driver name is always available once initialized
- `BackendFlagsAreMutuallyExclusive` - at most one of `isWayland`/`isX11`/`isXWaylandFallback`/`isWindows` is true
- `NativeWaylandIsNeverClassifiedAsXWaylandFallback` - regression test for the native-Wayland-vs-XWayland bug fixed in this PR; self-skips unless the actual driver is `"wayland"`
- `SupportsClientSideMaximizeIsFalseOnlyOnWayland`
- `SupportsGlobalMouseStateIsAlwaysTrue`
- `IsWindowsIsFalseOnNonWindowsBuilds` (Linux/macOS builds only)

**Run Unit Tests:**
```bash
# After successful configuration
cmake --build --preset win-debug  # Windows
cmake --build --preset debug      # Linux
ctest --preset win-debug          # Windows tests
ctest --preset debug              # Linux tests
```

**Expected Results:**
- All tests should pass
- No compilation warnings or errors
- Tests skip gracefully on platforms where they're not applicable

### Manual Test Matrix - Platform Combinations
- [ ] GNOME (latest stable)
  - [ ] Single-monitor setup
  - [ ] Multi-monitor setup (with and without different DPI)
- [ ] KDE Plasma (latest stable)
  - [ ] Single-monitor setup
  - [ ] Multi-monitor setup
- [ ] Sway (latest stable)
  - [ ] Single-monitor setup
  - [ ] Multi-monitor setup
- [ ] Hyprland (latest stable)
  - [ ] Single-monitor setup
  - [ ] Multi-monitor setup

#### Linux - XWayland Fallback
- [ ] GNOME Wayland with XWayland fallback enabled
  - [ ] Start tasksmack with `DISPLAY=:<N>` set alongside `WAYLAND_DISPLAY`
  - Verify behavior matches X11 (client-side positioning)

#### Linux - X11
- [ ] X11 with various window managers (verify no regressions)
  - [ ] XFCE
  - [ ] GNOME X11 session
  - [ ] KDE X11 session

#### Windows
- [ ] Windows 10/11 (verify no regressions)

### Test Scenarios for Each Platform

#### 1. **Window Maximize/Restore**
Test that borderless window maximize/restore works reliably:

**Wayland (Native):**
- [ ] Maximize window via double-click on title bar
  - Expected: Window maximizes smoothly using compositor
  - Verify: Window fills usable area, custom title bar visible
- [ ] Maximize via maximize button
  - Expected: Same as double-click
- [ ] Restore from maximized state via double-click
  - Expected: Window returns to previous size/position
- [ ] Restore via restore button
  - Expected: Same as double-click
- [ ] Multi-monitor: Maximize on primary, move to secondary, restore
  - Expected: Window restores to original position on primary

**X11/XWayland/Windows:**
- [ ] Maximize and restore using same tests as Wayland
  - Expected: Behavior should match previous versions (client-side positioning)

#### 2. **Window Dragging**
Test that title-bar dragging is smooth and accurate:

**Wayland (Native):**
- [ ] Click and drag title bar to move window
  - Expected: Window follows cursor smoothly, no jumps
  - Verify: No global mouse state artifacts
- [ ] Drag from maximized window (pending restore)
  - Expected: Window restores and begins following cursor after ~5px threshold
- [ ] Drag onto different monitor (multi-monitor)
  - Expected: Window moves smoothly between monitors
- [ ] Drag with window decorations hidden (if compositor supports)
  - Expected: Drag still works via custom title bar

**X11/XWayland/Windows:**
- [ ] Click and drag title bar (verify no regressions)
  - Expected: Same smooth behavior as before

#### 3. **Window Resizing (Edges)**
Test that custom border resize works:

**All Platforms:**
- [ ] Resize from top-left corner
  - Expected: Cursor changes to NESW resize; dragging resizes from top-left
- [ ] Resize from top edge
  - Expected: Cursor changes to NS resize; dragging changes only height
- [ ] Resize from right edge
  - Expected: Cursor changes to EW resize; dragging changes only width
- [ ] Resize from bottom-right corner
  - Expected: Cursor changes to NWSE resize; dragging resizes from bottom-right
- [ ] Drag across multiple monitors (multi-monitor)
  - Expected: Resize continues correctly across DPI boundaries

#### 4. **Double-Click Title Bar**
Test toggle maximize/restore:

**All Platforms:**
- [ ] Double-click on empty area of title bar
  - Expected: If not maximized, window maximizes; if maximized, restores
- [ ] Double-click while hovering over button (minimize/maximize/close)
  - Expected: Button action takes precedence (minimize/maximize/close)

#### 5. **Multi-Monitor Edge Cases**

**Wayland:**
- [ ] Maximize on monitor with scaling (125%, 150%, 200%)
  - Expected: Window uses correct usable bounds for that monitor
- [ ] Move window from scaled monitor to unscaled monitor
  - Expected: Window position/size updates correctly for new monitor's DPI
- [ ] Disconnect monitor with maximized window
  - Expected: Graceful fallback (compositor handles or window auto-restores)

**All Platforms:**
- [ ] Hot-plug/unplug monitors during maximize/drag
  - Expected: No crashes; window remains stable

#### 6. **Compositor-Specific Behaviors**

**GNOME:**
- [ ] Verify custom title bar integrates with GNOME's window management
- [ ] Test with GNOME's window tiling features (if active)

**KDE/Plasma:**
- [ ] Verify interaction with KDE's window snapping/tiling
- [ ] Test with multi-monitor panel/taskbar alignment

**Sway:**
- [ ] Verify correct behavior in tiling mode
- [ ] Test floating window mode

**Hyprland:**
- [ ] Verify animation and transition smoothness
- [ ] Test floating vs. tiling window modes

### Performance Validation

- [ ] No excessive CPU usage during drag/resize on any platform
- [ ] 60 FPS rendering maintained during interactive resize (Windows async resize, Wayland SSD)
- [ ] No stuttering or frame drops during window operations
- [ ] No memory leaks during repeated maximize/restore cycles (100+ iterations)

### Regression Testing

**Ensure no regressions in:**
- [ ] Process monitoring continues unaffected by window operations
- [ ] UI responsiveness remains >60 FPS during and after drag/resize
- [ ] Settings persistence (window position/size) works correctly
- [ ] Window close, minimize, other button actions work correctly

## Acceptance Criteria

**All of the following must pass for this PR to be merged:**

1. ✅ Code review passes (`/code-review` and `/tasksmack-review` skills)
2. ✅ Compilation succeeds (no warnings with clang-tidy)
3. ✅ Unit tests pass (if any added)
4. ✅ No regressions on Windows

**Before closing #382, the following evidence must be attached:**

1. Test results matrix showing ✅ passes for:
   - Maximize/Restore GNOME (Wayland) + multi-monitor
   - Maximize/Restore KDE (Wayland) + multi-monitor
   - Maximize/Restore Sway or Hyprland (Wayland) + multi-monitor
   - Maximize/Restore X11 fallback (XWayland)
   - Maximize/Restore Windows 10/11

2. Specific test evidence:
   - Screenshot or video showing:
     - Maximize/restore works smoothly on each platform
     - Drag operations follow cursor without jumps
     - Border resize works correctly
     - Multi-monitor scenarios work as expected
   - Crash logs or error logs (should be none or only informational)
   - Notes on any workarounds or deviations discovered

3. Backend detection logs:
   - Verify `VideoBackend` initialization logs correct driver name
   - Verify XWayland detection works (if tested)

## How to Run Tests

### Prerequisites
- Build tasksmack from the `mgradwohl-wayland-native-support` branch
- Enable resize performance tracing for diagnostics:
  ```bash
  TASKSMACK_TRACE_RESIZE_PERF=1 ./tasksmack
  ```

### Quick Smoke Test (All Platforms)
```bash
# Maximize, restore, drag, resize
./tasksmack &
# (perform manual test scenarios above)
```

### XWayland Fallback Testing (Linux)
```bash
# Start an X server on display :99 and a Wayland session on a nested socket
export DISPLAY=:99
export WAYLAND_DISPLAY=wayland-99
./tasksmack
# Verify VideoBackend logs show XWayland detection
```

## Known Limitations / Future Work

- **Native Wayland:** Some compositors may have quirks with client-side drag initiation. There is currently no user-facing setting to fall back to native window decorations if a compositor's behavior is problematic (see issue #745).
- **Multi-DPI:** Resize behavior on multi-DPI setups may vary by compositor. Log output will indicate which backend is active.
- **Touch input:** Not tested (future enhancement).

## Documentation References

- Architecture: tasksmack.md - Custom Title Bar Behavior section
- Code: `Core::VideoBackend` (src/Core/VideoBackend.h/cpp), `Window::maximize()`, `TitleBarLayer::getCurrentMousePosition()`
- Issue references: #593 (implementation), #382 (validation gate)
- Related: #373 (custom title bar intro, historical context)
