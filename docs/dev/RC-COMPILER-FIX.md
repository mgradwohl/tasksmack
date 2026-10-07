# Windows RC Compiler Fix (PR #743, hardened in PR #747, implementing issue #593)

## Problem

On Windows, TaskSmack's CMake configuration was failing with:
```
No CMAKE_RC_COMPILER could be found.
Tell CMake where to find the compiler by setting either the environment
variable "RC" or the CMake cache entry CMAKE_RC_COMPILER...
```

This prevented Windows development builds even when the Windows SDK was properly installed. Once
that was fixed by pre-detecting a compiler, a second problem surfaced: builds that did find the
Windows SDK's own `rc.exe` would hang indefinitely mid-build instead of failing fast (issue #746).

## Root Cause

CMake's Windows-Clang platform detection tries to enable the RC compiler during the `project()`
call without checking if `rc.exe` is in `PATH`. TaskSmack's Windows CI and `tools/setup-dev.ps1`
both install LLVM/Clang directly rather than launching from a Visual Studio Developer Command
Prompt, so `rc.exe` is never added to `PATH` even though it's present inside the Windows SDK
install.

Separately: this project builds with the plain Clang driver (not `clang-cl`), a toolchain pairing
that makes CMake bridge to MSVC's `rc.exe` via an internal two-stage helper
(`cmake -E cmake_llvm_rc`: Clang preprocesses the `.rc` file, then `rc.exe` compiles the
already-preprocessed output). That bridge hangs indefinitely for this project (issue #746:
reproduced repeatedly, both in CI and locally, always with a still-running `rc.exe` process using
0% CPU and no output) rather than failing fast -- running `rc.exe` standalone on the exact
preprocessed file CMake generates fails immediately instead of hanging, so the hang is specific to
CMake's bridging of the two processes, not to `rc.exe` or a missing `windows.h`/`INCLUDE` setup as
originally suspected.

## Solution

**Pre-detect an RC compiler before the `project()` call**, so CMake's own platform detection has a
valid compiler to find instead of failing outright, and **require `llvm-rc` specifically, with no
automatic fallback to `rc.exe`**:

1. If the caller pinned an RC compiler via `-DCMAKE_RC_COMPILER=...` or the `RC` environment
   variable (CMake's own convention, analogous to `CC`/`CXX`), skip auto-detection -- but see the
   stale-cache handling below, since `CMAKE_RC_COMPILER` also ends up cached by CMake's own
   machinery after any successful configure, not just by an explicit user choice.
2. Otherwise, prefer `llvm-rc`: this project's Windows toolchain is LLVM/Clang, not MSVC (see
   `cmake/CompilerOptions.cmake`), so `llvm-rc` is the natural fit, it ships with the LLVM install
   this project already requires, and CMake's `cmake_llvm_rc` bridge does not hang with it as the
   second stage (unlike MSVC's `rc.exe`). `llvm-rc` also resolves headers like `windows.h` via
   Clang's own MSVC/SDK auto-detection, the same mechanism Clang already uses for ordinary C++
   compiles, sidestepping the `INCLUDE`-environment-variable conflict from issue #686 -- so builds
   no longer need to run inside a Developer Command Prompt at all. Searched via `LLVM_ROOT/bin`
   (what the Windows presets/CI use to locate Clang), `%ProgramFiles%\LLVM\bin`, the hardcoded
   `C:\Program Files\LLVM\bin`, then `PATH`.
3. **No automatic fallback to the Windows SDK's `rc.exe`, `PATH`-found `rc.exe`, or
   `llvm-windres`.** LLVM (and `llvm-rc`) is already a required, always-installed prerequisite
   (`CONTRIBUTING.md`'s Windows Pre-Requisites, installed automatically by `tools/setup-dev.ps1`),
   so a missing `llvm-rc` only ever means a broken or incomplete dev environment -- never a
   platform that legitimately lacks an RC compiler, and never a reason to silently select the
   exact tool with the confirmed hang. If `llvm-rc` isn't found, **fail configuration with
   `FATAL_ERROR`** instead -- this matches the `FATAL_ERROR` checks already used for the `.rc.in`
   /manifest template files. (The DPI-awareness manifest itself doesn't depend on RC compilation
   at all -- it's embedded independently via `/MANIFESTINPUT` at link time; see `CMakeLists.txt`'s
   "Windows resource file" section.) To use a different RC compiler anyway (including `rc.exe`
   itself), opt in explicitly via `-DCMAKE_RC_COMPILER=<path>` or the `RC` environment variable --
   `RC` is the more durable choice specifically for `rc.exe`, since a fresh
   `-DCMAKE_RC_COMPILER=<path to rc.exe>` on a tree with no prior selection recorded yet is
   indistinguishable from a stale cached value from before this fix and is evicted rather than
   honored (see below), while `RC` is always honored.
4. **Stale-cache self-heal.** `CMAKE_RC_COMPILER` ends up cached after any successful configure,
   including a plain default-detected value from before this fix existed at all -- so "cached"
   alone can't distinguish a genuine override from a stale leftover, and CMake's normal
   `set(... CACHE ...)` semantics (which never overwrite an existing entry without `FORCE`) would
   otherwise leave such a tree stuck on `rc.exe` forever. An internal marker records this module's
   own last auto-selection so a cached value that still matches it is recognized as "ours" and
   safely re-detected, while a cached value that doesn't match it is normally trusted as a
   deliberate override -- except specifically when it's `rc.exe` by filename with no marker
   recorded yet, which is evicted instead, so a build tree configured before this fix self-heals
   on its next reconfigure without needing to be deleted.

### Implementation

See `cmake/RCCompiler.cmake` for the full pre-`project()` detection block (included from
`CMakeLists.txt` before `project()`), and the "Windows resource file" section of `CMakeLists.txt`
for the conditional resource compilation that consumes `CMAKE_RC_COMPILER`.

### Behavior

| Scenario | Result |
|----------|--------|
| `llvm-rc` found (the default case) | ✅ Full build with icon and version info (the manifest is always embedded independently, regardless of RC availability) |
| `-DCMAKE_RC_COMPILER=<path>` or `RC=<path>` set (not a fresh `rc.exe` path with no prior selection recorded) | ✅ That compiler is used as-is |
| `llvm-rc` not found, no override given | ❌ CMake configuration fails with `FATAL_ERROR` and installation instructions |
| Build tree has a stale cached `rc.exe` from before this fix | ✅ Self-heals to `llvm-rc` on the next reconfigure |

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

- **cmake/RCCompiler.cmake**: RC compiler detection, override handling, stale-cache self-heal, and fail-fast logic
- **CMakeLists.txt** ("Windows resource file" section): Conditional Windows resource compilation
- **CONTRIBUTING.md** (Windows Pre-Requisites section): Documents the LLVM/Windows SDK requirements and the RC compiler selection/override behavior
- **tools/setup-dev.ps1**: Installs LLVM and the Windows SDK via the VCTools workload
- **tools/check-prereqs.ps1**: Verifies an RC compiler is discoverable, mirroring `cmake/RCCompiler.cmake`'s `RC`-first, `llvm-rc`-only search
- **tests/Core/test_VideoBackend.cpp**: VideoBackend backend detection tests
