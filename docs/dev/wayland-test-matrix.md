# Wayland Native Support - Test Matrix and Validation

## Overview
This document defines the test matrix and acceptance criteria for the Wayland native support implementation (PR #743, implementing issue #593). All tests should be executed on each platform/compositor combination to validate that #865's manual testing gate is satisfied.

**Issue Workflow:**
- #593: Implementation umbrella issue, implemented by PR #743
- #865: Manual validation gate (consolidates the original #382 maximize/restore gate and #754's drag/title-bar-button-routing gate into one issue against this same matrix; manual test matrix evidence required before close)

## Implementation Summary
The implementation uses backend capability gating (`Core::VideoBackend`) to provide platform-specific behavior:
1. **Native Wayland:** Compositor-managed maximize/restore, event-consistent local-coordinate drag
2. **X11/XWayland:** Maximize asks the window manager when it supports EWMH maximize (#1339), otherwise client-side maximize/restore; global-coordinate drag (existing behavior preserved)
3. **Windows:** Title-bar drag handed to the OS move loop (`SDL_HITTEST_DRAGGABLE` -> `HTCAPTION`, so Aero Snap works, #2716); client-side maximize/restore and border resize (existing behavior preserved)

Backend classification is driven by `SDL_GetCurrentVideoDriver()`, not by which env vars happen to be set. Native Wayland is `driver == "wayland"`, checked first -- this matters because `DISPLAY` is commonly set on a Wayland session too (e.g. XWayland running for other apps), so testers should not assume "DISPLAY is set" means an X11/XWayland run. XWayland fallback is `driver == "x11"` *and* `WAYLAND_DISPLAY` set; plain X11 is `driver == "x11"` with `WAYLAND_DISPLAY` unset.

## Test Matrix

### Automated Unit Tests (CI/CD - Required for Merge)

`tests/Core/test_VideoBackend.cpp` validates `VideoBackend`'s classification and capability
queries -- the policy that gates platform-specific behavior. It does **not** exercise `Window`
maximize/restore or `TitleBarLayer` drag/resize behavior directly; those depend on live SDL
window/compositor state and are covered by the manual matrix below instead (drag on native
Wayland is handed to the compositor since #750, which fixed #744; on Windows to the OS move loop
since #2716). What the window's hit test answers -- and so who runs each gesture -- is covered:
`tests/App/test_TitleBarGeometry.cpp` (`ComputeWindowHitTestAtTest`: the empty bar, every button,
the icon, every edge and corner, maximized, at a 1.0 and a 2.0 display scale, for each policy) and
`tests/App/test_UiLayersOnDisplay.cpp` (`TheHitTestCallbackAnswersForTheRealWindow`: the callback
installed on the real window, asked through `TitleBarLayer::hitTest()` and, on Windows, through
`WM_NCHITTEST`; `TitleBarDragGeometryKeepsThePointerOnTheWindow` records the answer at each of its
press points beside the client-side path it drives).

#### VideoBackend Detection Tests (tests/Core/test_VideoBackend.cpp)

These tests verify the backend detection and capability query system that gates all platform-specific behavior.

**Test Coverage:**
- `ClassifyBackendTest` (6 cases) - table-tests `VideoBackend::classifyBackend(driverName, waylandDisplaySet)` directly. It's a pure function with no SDL/environment calls of its own, so every backend and edge case (empty/unrecognized driver name, Wayland regardless of env, X11 with/without `WAYLAND_DISPLAY`, Windows per-platform) is exercised deterministically regardless of what driver the test process actually has active.
- `ShouldUseBorderlessTitleBarTest` (3 cases) - table-tests `VideoBackend::shouldUseBorderlessTitleBar(forceNativeDecorationsOnWayland, isWayland)`: the custom title bar is used unless the native-decorations opt-in is on *and* the backend is native Wayland (#745).
- `VideoBackendTest` (8 `TEST_F` cases) - exercises the real `initialize()`/`isWayland()`/etc. path against whichever real or headless SDL driver is active, so (unlike `ClassifyBackendTest`) these check cross-backend invariants rather than asserting a specific backend on every platform:
  - `InitializeIsIdempotent` - calling `initialize()` twice doesn't change the result
  - `DriverNameIsNotEmptyAfterInitialize` - driver name is always available once initialized
  - `BackendFlagsAreMutuallyExclusive` - at most one of `isWayland`/`isX11`/`isXWaylandFallback`/`isWindows` is true
  - `ResetForTestingAllowsReinitialization` - `resetForTesting()` clears the cached backend, and a second `initialize()` detects the same driver again
  - `NativeWaylandIsNeverClassifiedAsXWaylandFallback` - regression test for the native-Wayland-vs-XWayland bug fixed in this PR; self-skips unless the actual driver is `"wayland"`
  - `SupportsClientSideMaximizeIsFalseOnlyOnWayland`
  - `SupportsGlobalMouseStateIsFalseOnlyOnWayland` - global mouse state is deliberately unsupported on native Wayland (`TitleBarLayer` uses the window-local query there instead)
  - `UsesSystemDragIsTrueOnlyOnWaylandAndWindows` / `UsesSystemResizeIsFalseOnlyOnWindows` - the live backend's hit-test policy (#2716)
  - `IsWindowsIsFalseOnNonWindowsBuilds` (Linux/macOS builds only)
- `HitTestPolicyTest` (2 cases) - table-tests `VideoBackend::usesSystemDrag(Backend)` / `usesSystemResize(Backend)`: the OS or compositor runs the title-bar drag on native Wayland and Windows only, and the border resize everywhere but Windows (#2716).

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
  - [ ] Start TaskSmack with `DISPLAY=:<N>` and `WAYLAND_DISPLAY` set, **and** `SDL_VIDEODRIVER=x11` -- setting the display variables alone does not select XWayland; SDL still normally chooses its native Wayland driver
  - [ ] Verify VideoBackend logs confirm the `x11` driver before recording results
  - Verify behavior matches X11 (client-side positioning)

#### Linux - X11
- [ ] X11 with various window managers (verify no regressions)
  - [ ] XFCE
  - [ ] GNOME X11 session
  - [ ] KDE X11 session

#### Windows
- [ ] Windows 10/11 (verify no regressions)
  - [ ] Single-monitor at 100% and at 150% DPI, and a two-monitor setup (the move loop is the OS's since #2716; see scenario 2)

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
  - Expected: Behavior should match previous versions (client-side positioning on Windows, and on
    X11 window managers without EWMH maximize)
- [ ] X11/XWayland with a panel or taskbar (GNOME on Xorg, KDE, XFCE, WSLg): maximize with the
  title-bar button, then with the window manager's own shortcut or menu (#1339)
  - Expected: both fill the same rectangle and stop at the panel/taskbar

#### 2. **Window Dragging**
Test that title-bar dragging is smooth and accurate:

**Wayland (Native):** drag is delegated to the compositor (`SDL_HITTEST_DRAGGABLE` -> `xdg_toplevel_move()`, #744) rather than the client-side `updateDrag()` path X11/XWayland use -- the "~5px threshold before un-maximizing" and "no global mouse state artifacts" behaviors described for other platforms below do not apply here; the compositor owns the whole gesture once it starts.
- [ ] Click and drag the empty title-bar area (not a button) to move window
  - Expected: Window follows cursor smoothly via the compositor's own move gesture
- [ ] Drag from a maximized window
  - Expected: Compositor's own maximize/restore-on-drag behavior applies (varies by compositor -- note what actually happens)
- [ ] Drag onto a different monitor (multi-monitor)
  - Expected: Window moves smoothly between monitors
- [ ] **Regression check: every title-bar button still works** (icon/system-menu, about, help, settings, minimize, maximize, close) -- these must stay clickable and must NOT start a window drag. This is the highest-risk regression surface for #744's fix (`isPointInControlArea()` must be checked before returning `DRAGGABLE`).
- [ ] Double-click the empty title-bar area
  - Expected (accepted trade-off, not a bug): does NOT toggle maximize/restore on native Wayland, since the compositor consumes the button-down entirely. Use the maximize button instead.

**X11/XWayland:**
- [ ] Click and drag title bar (verify no regressions)
  - Expected: Same smooth client-side behavior as before, including the maximized-drag restore threshold
- [ ] Double-click the title bar
  - Expected: Toggles maximize/restore (unaffected by the native-Wayland change above)

**Windows (#2716):** a press on the empty title bar of a normal window is `HTCAPTION`, so Windows runs the move itself; the app's own drag (`updateDrag()`) runs only from a client-side maximized window, where the OS would otherwise drag the work-area-sized window around instead of restoring it. The borders stay the app's resize.
- [ ] Click and drag the empty title bar
  - Expected: the window follows the pointer; the app's frames pause for the duration of the drag (Windows' move loop is modal) and resume on release
- [ ] Drag to the left or right screen edge
  - Expected: the snap preview appears and the window snaps to that half on release; dragging it away again restores its size
- [ ] Drag to the top edge
  - Expected: the window maximizes (the OS maximize is adopted as the client-side one, #1208); the title bar shows Restore
- [ ] Win+Left / Win+Right / Win+Up / Win+Down
  - Expected: snap left/right, maximize, restore/minimize as for a native window
- [ ] Drag a maximized window's title bar
  - Expected: past ~5px the window is restored under the pointer and keeps following it (client-side, #2715); it does not snap in that same gesture -- release and drag again to snap
- [ ] Double-click the empty title bar
  - Expected: toggles maximize/restore (Windows' own caption double-click, then the app's for the maximized window)
- [ ] Hover and click every button (icon/system menu, About, Help, Settings, Minimize, Maximize, Close)
  - Expected: each works; none starts a drag (buttons stay `SDL_HITTEST_NORMAL`)
- [ ] Hover the Maximize button
  - Expected (known limitation): no Snap Layouts flyout; SDL has no `HTMAXBUTTON` hit-test result
- [ ] Two monitors, one at 150% DPI: drag the window between them and snap on each
  - Expected: the drag crosses over and the snap targets the monitor the pointer is on

#### 3. **Window Resizing (Edges)**
Test that custom border resize works:

**All Platforms:**
- [ ] Resize from top-left corner
  - Expected: Cursor changes to NESW resize; dragging resizes from top-left
- [ ] Resize from top-right corner, inside the Close button's slot (#2882)
  - Expected: Cursor changes to NESW resize over the corner square (the resize border's thickness on a side) and the X does not highlight; dragging resizes from top-right. One pixel left of or below the square the arrow cursor is back and a click closes the window
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

**X11/XWayland/Windows:**
- [ ] Double-click on empty area of title bar
  - Expected: If not maximized, window maximizes; if maximized, restores
- [ ] Double-click while hovering over button (minimize/maximize/close)
  - Expected: Button action takes precedence (minimize/maximize/close)

**Native Wayland:**
- [ ] Double-click on empty area of title bar
  - Expected (accepted trade-off, not a bug -- see #744): does nothing; the compositor consumes the button-down for the drag gesture before the app can see it as a double-click. Use the maximize button instead.
- [ ] Double-click while hovering over a button (minimize/maximize/close)
  - Expected: Button action still takes precedence and works normally (buttons stay `SDL_HITTEST_NORMAL` on every backend)

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
  - Expected: launched tiled, the window renders at the tile's full size, and keeps it when a tab switch or a font-size change re-sets the minimum window size (#1777). GNOME and KDE half-screen snaps use the same tiled states.
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
- [ ] Settings persistence (window position/size) works correctly -- on native Wayland only the size and maximized state are restored, since the app can't position its window there (`Window::supportsPositioning()`)
- [ ] Window close, minimize, other button actions work correctly

## Acceptance Criteria

**All of the following had to pass for PR #743 to merge (it has, as of commit `0836da4`):**

1. ✅ Code review passes (`/code-review` and `/tasksmack-review` skills)
2. ✅ Compilation succeeds, no clang-tidy warnings, on Linux and Windows (#746 fixed by switching RC compiler to `llvm-rc`)
3. ✅ Unit tests pass: 1284/1284 on Linux, 1159/1159 on Windows (both 100%)
4. ✅ No regressions on Windows -- confirmed on CI run 33661250717 after the #746 fix

**Before closing #865, the following evidence must be attached:**

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
- Build TaskSmack from `main` (for example `cmake --workflow --preset dev`, or the `release` preset); the binary is `build/<preset>/bin/TaskSmack`. The commands below run it from that directory.
- Enable resize performance tracing for diagnostics:
  ```bash
  TASKSMACK_TRACE_RESIZE_PERF=1 ./TaskSmack
  ```

### Quick Smoke Test (All Platforms)
```bash
# Maximize, restore, drag, resize
./TaskSmack &
# (perform manual test scenarios above)
```

### XWayland Fallback Testing (Linux)
```bash
# Setting DISPLAY and WAYLAND_DISPLAY alone does NOT force SDL onto X11 -- inside a real
# Wayland session, SDL still prefers its native "wayland" driver and DISPLAY is commonly
# set there anyway (e.g. via XWayland running for other apps), so this would silently test
# the wrong backend. Force the X11 driver explicitly with SDL_VIDEODRIVER.
export DISPLAY=:99
export WAYLAND_DISPLAY=wayland-99
export SDL_VIDEODRIVER=x11
./TaskSmack
# Verify the logged driver is actually "x11" (VideoBackend logs "Detected XWayland fallback")
# before recording results -- if it logs native Wayland instead, SDL_VIDEODRIVER wasn't honored
# and results from this run don't test XWayland at all.
```

## Known Limitations / Future Work

- **Native Wayland:** Title-bar dragging is delegated to the compositor via `SDL_HITTEST_DRAGGABLE` (see #744); as a consequence, double-click-to-maximize does not fire from the title bar there (the maximize button remains available). Users who hit compositor-specific drag/resize quirks can switch to native window decorations via Settings > Advanced > "Use native window decorations instead of the custom title bar" (see #745; native Wayland only, takes effect on next launch).
- **Windows:** Title-bar dragging is the OS move loop (`HTCAPTION`, #2716), so Aero Snap works, but Snap Layouts on the maximize button does not (SDL has no `HTMAXBUTTON` result), a drag from a maximized window restores it client-side and does not snap in the same gesture, and the app's frames pause while Windows' modal move loop runs. The border resize stays client-side for the same reason in reverse: the OS resize loop would freeze the window's contents until release.
- **Multi-DPI:** Resize behavior on multi-DPI setups may vary by compositor. Log output will indicate which backend is active.
- **Touch input:** Not tested (future enhancement).

## Documentation References

- Architecture: tasksmack.md - Application and Rendering Lifecycle section (custom title-bar behavior per backend)
- Code: `Core::VideoBackend` (src/Core/VideoBackend.h/cpp, `usesSystemDrag()`/`usesSystemResize()`), `Window::maximize()`, `TitleBarLayer::queryMouseState()`, `TitleBarLayer::hitTest()`/`hitTestInput()`/`updateResizeCursor()` (src/App/TitleBarLayer.cpp), `computeWindowHitTestAt()`/`computeWindowHitTest()`/`computeSystemDragApplies()`/`computeTitleBarAreaHitTest()` (src/App/TitleBarGeometry.h)
- Issue references: #593 (implementation), #865 (validation gate, consolidating #382 and #754), #744 (native Wayland drag delegation), #745 (native-decorations opt-in setting), #749 (resize-cursor hover regression), #750 (drag/title-bar-button routing fix), #2716 (Windows title-bar drag handed to the OS move loop: Aero Snap)
- Related: #373 (custom title bar intro, historical context)
