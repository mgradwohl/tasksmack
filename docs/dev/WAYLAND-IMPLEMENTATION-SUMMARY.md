# Wayland Native Support

Adds a `Core::VideoBackend` capability gate (PR #743, implementing issue #593) so `Window` and
`TitleBarLayer` use compositor-safe behavior on native Wayland while preserving existing
client-side behavior on X11/XWayland/Windows.

This document previously carried a full narrative summary, but it drifted from the actual
implementation three separate times across this PR's review (most recently: it still described
XWayland detection as "both `WAYLAND_DISPLAY` and `DISPLAY` set" after that had already been
fixed elsewhere, and still cited an already-retracted "FreeType RC compilation" explanation for
CI failures that were actually caused by issue #746). Rather than fix the same class of drift a
fourth time, this is now a thin pointer to the documents that stay current because the rest of
the codebase (and CI) actually depends on them being right:

- **Architecture and backend detection logic:** `tasksmack.md`
- **Windows RC compiler detection:** `RC-COMPILER-FIX.md`, `cmake/RCCompiler.cmake` (included from `CMakeLists.txt` before `project()`)
- **Manual validation matrix:** `wayland-test-matrix.md`
- **Prerequisites:** `CONTRIBUTING.md`
- **Automated tests:** `tests/Core/test_VideoBackend.cpp`

## Outstanding work

- **#865** — manual Wayland/XWayland/X11 validation gate (it replaced #382, now closed); `wayland-test-matrix.md` is defined but not yet run on real compositors

Done since:

- **#744** — title-bar drag on native Wayland: fixed by #750, which hands the drag to the compositor (`SDL_HITTEST_DRAGGABLE`)
- **#745** — native-decorations fallback on Wayland: Settings > Advanced > "Use native window decorations instead of the custom title bar" (#767)
- **#746** — `build-windows-debug` hung compiling `tasksmack.rc` with the Windows SDK's `rc.exe`; fixed by requiring `llvm-rc`
