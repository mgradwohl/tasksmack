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

**Pre-detect RC.exe before the `project()` call**, so CMake's own platform detection has a valid compiler to find instead of failing outright:

1. Glob for `rc.exe` under any installed Windows SDK version (`Windows Kits/10/bin/*/x64` and `Windows Kits/11/bin/*/x64`, under both `Program Files` and `Program Files (x86)`), preferring the highest version found. The exact SDK version isn't hardcoded because it changes whenever the SDK updates -- including unannounced bumps to the `windows-2025` GitHub Actions runner image.
2. If nothing matches, fall back to `PATH` (covers a Developer Command Prompt or a custom install), then `llvm-rc`, then `llvm-windres`.
3. If none of those find an RC tool, **fail configuration with `FATAL_ERROR`**. The Windows SDK is a documented, required prerequisite (`CONTRIBUTING.md`'s Windows Pre-Requisites) installed automatically by `tools/setup-dev.ps1`, so "not found" only ever means a broken or incomplete dev environment -- never a platform that legitimately lacks an RC compiler. Silently shipping a `.exe` with no icon, no version info, and no DPI-awareness manifest is a real regression that's easy to miss in build logs; failing loudly matches the `FATAL_ERROR` checks already used for the `.rc.in`/manifest template files.

### Implementation

See `cmake/RCCompiler.cmake` for the full pre-`project()` detection block (included from `CMakeLists.txt` before `project()`), and the "Windows resource file" section of `CMakeLists.txt` for the conditional resource compilation that consumes `CMAKE_RC_COMPILER`.

### Behavior

| Scenario | Result |
|----------|--------|
| RC.exe found in SDK, PATH, llvm-rc, or llvm-windres | ✅ Full build with resources (icon, version info, manifest) |
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
