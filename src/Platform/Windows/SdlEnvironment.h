#pragma once

// What an elevated TaskSmack.exe takes from SDL's environment variables (#4103): nothing. SDL reads a hint it isn't
// given from the variable of the same name, and some hints name a DLL it loads by full path, which no DLL search
// order restricts (DllSearchPolicy.h covers only a DLL loaded by name): SDL_OPENGL_LIBRARY, which the OpenGL window
// loads, SDL_EGL_LIBRARY, SDL_VULKAN_LIBRARY, SDL_OPENVR_LIBRARY and SDL_X11_XCB_LIBRARY. SDL3_DYNAMIC_API, which
// SDL's dynamic API reads with GetEnvironmentVariableA at its first call, static build or not, replaces SDL itself
// with the DLL it names. A same-user elevation reads the user's own HKCU\Environment, which the unelevated side can
// set, so under DllSearchPolicy's System32-only rule WinMain removes every such variable from its environment before
// it makes any SDL call. The hints TaskSmack sets itself, with SDL_SetHint, still apply.
//
// Every variable is removed, not just the ones that name a file: the others pick the video driver (SDL_VIDEO_DRIVER
// can pick one that loads a library) and other behaviour, none of which an elevated TaskSmack should take from the
// less-privileged side either, and a list of the risky ones would go stale as SDL adds hints.
//
// The pure functions take the environment block as GetEnvironmentStringsW() gives it and a removal callback, so
// test_SdlEnvironment.cpp covers every case with a fake one; removeForThisProcess() works on the process's own.

#ifdef _WIN32

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows::SdlEnvironment
{

/// Whether @p name is a variable SDL 3 reads: "SDL_" or "SDL" then digits then "_" (SDL3_DYNAMIC_API), in any case,
/// as Windows matches environment variable names.
[[nodiscard]] bool isSdlVariable(std::wstring_view name) noexcept;

/// The names of the SDL variables in @p block, an environment block as GetEnvironmentStringsW() gives it
/// ("NAME=value" strings, each ending in a null, the last followed by another), in the order they appear. The
/// per-drive "=C:=C:\..." entries are skipped. Reading stops at the end of @p block if no empty string ends it first.
[[nodiscard]] std::vector<std::wstring> sdlVariableNames(std::wstring_view block);

/// Removes the SDL variables in @p block when @p system32Only (DllSearchPolicy::system32Only()), calling @p remove
/// with each name; otherwise leaves them alone and calls nothing. Returns the names @p remove reported removed.
[[nodiscard]] std::vector<std::wstring>
removeSdlVariables(bool system32Only, std::wstring_view block, const std::function<bool(const std::wstring&)>& remove);

/// removeSdlVariables() on this process's environment: GetEnvironmentStringsW(), and SetEnvironmentVariableW(name,
/// nullptr) for each; if the block can't be read, the variables that name a library or pick the video driver, by
/// name, so it fails closed. WinMain calls it right after SetDefaultDllDirectories, before any SDL call, and logs the names
/// it returns once logging is up (removedFromThisProcess()). Never their values. Not thread-safe: WinMain's alone.
std::vector<std::wstring> removeForThisProcess(bool system32Only);

/// The names the last removeForThisProcess() removed, in UTF-8, for main.cpp to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

} // namespace Platform::Windows::SdlEnvironment
#endif
