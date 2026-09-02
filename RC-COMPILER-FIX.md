# Windows RC Compiler Fix (PR #743, implementing issue #593)

## Problem

On Windows, TaskSmack's CMake configuration was failing with:
```
No CMAKE_RC_COMPILER could be found.
Tell CMake where to find the compiler by setting either the environment
variable "RC" or the CMake cache entry CMAKE_RC_COMPILER...
```

This prevented Windows development builds even when the Windows SDK was properly installed.

## Root Cause

CMake's Windows-Clang platform detection tries to enable the RC compiler during the `project()` call without checking if `rc.exe` is in PATH. TaskSmack's Windows CI and `tools/setup-dev.ps1` both install LLVM/Clang directly rather than launching from a Visual Studio Developer Command Prompt, so `rc.exe` is never added to `PATH` even though it's present inside the Windows SDK install.

## Solution

**Pre-detect an RC compiler before the `project()` call**, so CMake's own platform detection has a valid compiler to find instead of failing outright:

1. Prefer `llvm-rc`: this project's Windows toolchain is LLVM/Clang, not MSVC (see `cmake/CompilerOptions.cmake`), so `llvm-rc` is the natural fit, it ships with the LLVM install this project already requires, and -- concretely -- the Windows SDK's own `rc.exe` is confirmed to hang indefinitely mid-build in this project's CI (issue #746: reproduced 3x, always timing out at the 35-minute job limit with a still-running `rc.exe` process and zero output). Because the SDK's `rc.exe` used to be tried first and was always found (the SDK is a required, always-installed prerequisite here), `llvm-rc` was never actually exercised in practice, so this hang went undiagnosed until RC-compiler auto-detection was added at all.
2. If `llvm-rc` isn't found (e.g. a non-standard LLVM install), glob for `rc.exe` under any installed Windows SDK version (`Windows Kits/10/bin/*/x64` and `Windows Kits/11/bin/*/x64`, under both `Program Files` and `Program Files (x86)`), preferring the highest version found. The exact SDK version isn't hardcoded because it changes whenever the SDK updates -- including unannounced bumps to the `windows-2025` GitHub Actions runner image.
3. If nothing matches, fall back to `PATH` (covers a Developer Command Prompt or a custom install), then `llvm-windres`.
4. If none of those find an RC tool, **fail configuration with `FATAL_ERROR`**. LLVM and the Windows SDK are both documented, required prerequisites (`CONTRIBUTING.md`'s Windows Pre-Requisites) installed automatically by `tools/setup-dev.ps1`, so "not found" only ever means a broken or incomplete dev environment -- never a platform that legitimately lacks an RC compiler. Silently shipping a `.exe` with no icon and no version info is a real regression that's easy to miss in build logs; failing loudly matches the `FATAL_ERROR` checks already used for the `.rc.in`/manifest template files. (The DPI-awareness manifest itself doesn't depend on RC compilation at all -- it's embedded independently via `/MANIFESTINPUT` at link time; see `CMakeLists.txt`'s "Windows resource file" section.)

### Implementation

See `cmake/RCCompiler.cmake` for the full pre-`project()` detection block (included from `CMakeLists.txt` before `project()`), and the "Windows resource file" section of `CMakeLists.txt` for the conditional resource compilation that consumes `CMAKE_RC_COMPILER`.

### Behavior

| Scenario | Result |
|----------|--------|
| llvm-rc, SDK rc.exe, PATH rc.exe, or llvm-windres found | ✅ Full build with icon and version info (the manifest is always embedded independently, regardless of RC availability) |
| No RC tool found anywhere | ❌ CMake configuration fails with `FATAL_ERROR` and installation instructions |

## Testing

### Configure-Only Test (No Build)
```bash
cmake --preset win-debug
# Should succeed with a "Found RC compiler:" message
```

### Full Build
```bash
cmake --workflow --preset win-dev
```

### VideoBackend Unit Tests
```cpp
// tests/Core/test_VideoBackend.cpp
// Tests for:
// - Backend detection consistency (Wayland, X11, XWayland, Windows flags are
//   mutually exclusive; behavior is driver-agnostic since real CI runners don't
//   expose a real Wayland/X11 display)
// - Capability queries (supportsClientSideMaximize, supportsGlobalMouseState)
// - initialize() idempotency
```

Run tests with:
```bash
ctest --preset win-debug -R VideoBackend
```

## Related Files

- **cmake/RCCompiler.cmake**: RC compiler detection and fail-fast logic
- **CMakeLists.txt** ("Windows resource file" section): Conditional Windows resource compilation
- **CONTRIBUTING.md** (Windows Pre-Requisites section): Documents the Windows SDK requirement
- **tools/setup-dev.ps1**: Installs the Windows SDK via the VCTools workload
- **tools/check-prereqs.ps1**: Verifies `rc.exe` is discoverable
- **tests/Core/test_VideoBackend.cpp**: VideoBackend backend detection tests
