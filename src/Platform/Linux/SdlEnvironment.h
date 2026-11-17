#pragma once

// What a TaskSmack run as root for a user takes from SDL's environment variables (#4111, the Linux side of #4103):
// nothing. Its environment is the user's (sudo -E, or an env_keep list that keeps them), and SDL reads a hint it isn't
// given from the variable of the same name. Some name a library SDL dlopen()s, as root: SDL_OPENGL_LIBRARY (the X11
// GL loader and the EGL path Wayland uses), SDL_EGL_LIBRARY, SDL_X11_XCB_LIBRARY, SDL_VULKAN_LIBRARY. SDL3_DYNAMIC_API,
// which SDL's dynamic API reads with getenv() at its first call, static build or not, replaces SDL itself with the
// library it names. glibc's secure-execution mode doesn't apply: a sudo session isn't set-user-ID for TaskSmack.
//
// So main() removes every SDL variable from its environment (unsetenv) before its first SDL call, in a session run as
// root for a user (SessionUser::invokingUserHome(), the rule #4090 and #4104 use), and runApp() logs their names once
// logging is up, never their values. Every one, not a list of the risky ones, which would go stale as SDL adds hints;
// the hints TaskSmack sets itself, with SDL_SetHint, still apply. Plain sudo (env_reset) and pkexec clear them anyway.
//
// The logic takes the environment and the unset call as arguments, so test_LinuxSdlEnvironment.cpp covers it with a
// fake one; removeForThisProcess() works on the process's own.

#ifdef __linux__

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::SdlEnvironment
{

/// Whether @p name is a variable SDL 3 reads: "SDL_", or "SDL" then digits then "_" (SDL3_DYNAMIC_API, SDL2's older
/// names). Exact case, as Linux compares environment variable names.
[[nodiscard]] bool isSdlVariable(std::string_view name) noexcept;

/// The names of the SDL variables in @p environment ("NAME=value" strings, null-terminated, as environ is), in order.
/// An entry with no '=' is taken whole as a name.
[[nodiscard]] std::vector<std::string> sdlVariableNames(const char* const* environment);

/// Removes each SDL variable in @p environment with @p unset. @return The names removed (a failed unset is left out).
/// The names are collected first, so unsetting while walking environ can't skip one.
[[nodiscard]] std::vector<std::string> removeSdlVariables(const char* const* environment,
                                                          const std::function<bool(const std::string&)>& unset);

/// main()'s call, before any SDL call: when @p remove, removes every SDL variable from this process's environment.
/// Not thread-safe (unsetenv), so first thing in main().
void removeForThisProcess(bool remove);

/// The names removeForThisProcess() removed, for runApp() to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

} // namespace Platform::SdlEnvironment

#endif
