#pragma once

// What an elevated TaskSmack.exe takes from SDL's environment variables (#4103): nothing. SDL reads a hint it isn't
// given from the variable of the same name, and some hints name a DLL it loads by full path, which no DLL search
// order restricts (DllSearchPolicy.h covers only a DLL loaded by name): SDL_OPENGL_LIBRARY, which the OpenGL window
// loads, SDL_EGL_LIBRARY, SDL_VULKAN_LIBRARY, SDL_OPENVR_LIBRARY and SDL_X11_XCB_LIBRARY. SDL3_DYNAMIC_API, which
// SDL's dynamic API reads with GetEnvironmentVariableA at its first call, static build or not, replaces SDL itself
// with the DLL it names. A same-user elevation reads the user's own HKCU\Environment, which the unelevated side can
// set, so under DllSearchPolicy's System32-only rule WinMain removes every such variable from its environment before
// it makes any SDL call (tools/check-dll-loads.py checks it is WinMain's next statement after
// SetDefaultDllDirectories). The hints TaskSmack sets itself, with SDL_SetHint, still apply.
//
// Every variable is removed, not just the ones that name a file: the others pick the video driver (SDL_VIDEO_DRIVER
// can pick one that loads a library) and other behaviour, none of which an elevated TaskSmack should take from the
// less-privileged side either, and a list of the risky ones would go stale as SDL adds hints.
//
// Two passes. The block's SDL variables, matched by an ASCII case fold; then the dangerous ones (BY_NAME_VARIABLES)
// by name, asking the OS, whose own case-insensitive comparison of names is Unicode's: a variant the ASCII fold
// misses ("ſDL3_DYNAMIC_API", with U+017F) is still one GetEnvironmentVariableA("SDL3_DYNAMIC_API") finds. The
// second pass also runs alone, and so fails closed, when the block can't be read.
//
// Which copy: the Win32 environment block, which is all SDL reads on Windows (SDL_CreateEnvironment() takes
// GetEnvironmentStringsW(), SDL_getenv_unsafe() and the dynamic API take GetEnvironmentVariableA()), and what child
// processes inherit. The CRT's own copy (getenv, _wgetenv), made from the block at start-up, is cleared too, best
// effort, with _wputenv_s(name, L""), so nothing in TaskSmack that reads it sees a removed variable either.
//
// The logic takes the block and the environment's operations as arguments, so test_SdlEnvironment.cpp covers every
// case with a fake one; removeForThisProcess() works on the process's own.

#ifdef _WIN32

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows::SdlEnvironment
{

/// The variables removed by name as well as from the block: those that name a library SDL 3 loads (its *_LIBRARY
/// hints, and SDL3_DYNAMIC_API), pick its video driver (SDL_VIDEO_DRIVER, and SDL2's SDL_VIDEODRIVER, which SDL 3
/// still reads), or that its dynamic API reads besides (SDL_DYNAPI_LOG_CALLS).
inline constexpr std::array<std::wstring_view, 9> BY_NAME_VARIABLES = {
    L"SDL3_DYNAMIC_API",
    L"SDL_DYNAPI_LOG_CALLS",
    L"SDL_OPENGL_LIBRARY",
    L"SDL_EGL_LIBRARY",
    L"SDL_VULKAN_LIBRARY",
    L"SDL_OPENVR_LIBRARY",
    L"SDL_X11_XCB_LIBRARY",
    L"SDL_VIDEO_DRIVER",
    L"SDL_VIDEODRIVER",
};

/// The longest name, in UTF-16 code units, logName() keeps.
inline constexpr std::size_t MAX_LOGGED_NAME = 128;

/// The most names removedFromThisProcess() keeps; after them it has one "and N more".
inline constexpr std::size_t MAX_LOGGED_NAMES = 32;

/// What stillSetInThisProcess() holds when GetEnvironmentStringsW() failed: the block's SDL variables can't be found.
inline constexpr std::wstring_view UNREADABLE_BLOCK = L"(environment block unreadable)";

/// What removeSdlVariables() does to an environment: the process's own, or a test's fake.
struct EnvironmentOps
{
    std::function<bool(const std::wstring&)> isSet;  ///< Whether a variable of that name is set (the OS's comparison)
    std::function<bool(const std::wstring&)> remove; ///< Removes it; false if that failed
};

/// Whether @p name is a variable SDL 3 reads: "SDL_" or "SDL" then digits then "_" (SDL3_DYNAMIC_API), in any ASCII
/// case, as Windows matches environment variable names.
[[nodiscard]] bool isSdlVariable(std::wstring_view name) noexcept;

/// The names of the SDL variables in @p block, an environment block as GetEnvironmentStringsW() gives it
/// ("NAME=value" strings, each ending in a null, the last followed by another), in the order they appear. The
/// per-drive "=C:=C:\..." entries are skipped. Reading stops at the end of @p block if no empty string ends it first.
[[nodiscard]] std::vector<std::wstring> sdlVariableNames(std::wstring_view block);

/// When @p system32Only (DllSearchPolicy::system32Only()): removes the SDL variables in @p block, then each of
/// BY_NAME_VARIABLES while @p environment says one of that name is still set (a bounded number of times). With no
/// @p block (it couldn't be read), only the second. Returns the names removed: from the block, each @p environment
/// removed; by name, each it found set and then removed at least once. Otherwise leaves everything alone and calls nothing.
/// Each name @p environment still says is set when its passes are done (a removal that failed, one that took another
/// entry, or more entries answering to the name than the bounded passes remove) is added to @p stillSet, if given,
/// once: the caller fails closed. Only the Win32 block counts, since SDL reads nothing else: a leftover in the CRT's
/// copy alone is deliberately not fatal.
[[nodiscard]] std::vector<std::wstring> removeSdlVariables(bool system32Only,
                                                           std::optional<std::wstring_view> block,
                                                           const EnvironmentOps& environment,
                                                           std::vector<std::wstring>* stillSet = nullptr);

/// @p name as it may be logged: UTF-8, with each control character (C0, DEL, C1, and U+2028/U+2029, which end a line)
/// written as "\xNN" or "\uNNNN", as is each bidirectional control (U+200E, U+200F, U+202A to U+202E, U+2066 to
/// U+2069) that could reorder the log around the name, each backslash written as two so an escape can't be forged
/// with a literal one, and cut to MAX_LOGGED_NAME code units with "..." after, so a name set in
/// HKCU\Environment can't forge a log line.
[[nodiscard]] std::string logName(std::wstring_view name);

/// At most MAX_LOGGED_NAMES of @p names through logName(), then "and N more" if there are others.
[[nodiscard]] std::vector<std::string> loggedNames(const std::vector<std::wstring>& names);

/// The environment block as @p charAt reads it (GetEnvironmentStringsW()'s: "NAME=value" strings, each ending in a null,
/// the last followed by another), up to and including the last string's null. An empty environment is one lone null: the
/// character after it is never read, so a block that is only a terminator can't be read past. @p charAt is asked for
/// index n+1 only after index n was a null that ends a string, so the block's own terminator is the farthest it reads.
[[nodiscard]] std::wstring copyEnvironmentBlock(const std::function<wchar_t(std::size_t)>& charAt);

/// removeSdlVariables() on this process's environment: GetEnvironmentStringsW(), GetEnvironmentVariableW() and
/// SetEnvironmentVariableW(name, nullptr), with _wputenv_s(name, L"") for the CRT's copy. WinMain calls it right after
/// SetDefaultDllDirectories, before any SDL call, and runApp() logs the names it removed once logging is up
/// (removedFromThisProcess()). Never their values. Not thread-safe: WinMain's alone.
std::vector<std::wstring> removeForThisProcess(bool system32Only);

/// The names the last removeForThisProcess() removed, as loggedNames() gives them, for main.cpp to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

/// The names the last removeForThisProcess() could not remove: still set when it was done. Not empty means the
/// process may still take an SDL setting, or a library, from the less-privileged side, so WinMain refuses to start
/// (logging isn't up; it says so in a message box and OutputDebugStringW). Also holds UNREADABLE_BLOCK if the
/// environment block couldn't be read. Reachable only if the OS refuses a removal, a removal reports success but takes
/// a different entry (two names that compare equal), or more than eight entries answer to one name; not through
/// HKCU\Environment, whose names compare case-insensitively. An unreadable block refuses to start too, and so does
/// --handoff without elevation, which also scrubs.
[[nodiscard]] const std::vector<std::wstring>& stillSetInThisProcess() noexcept;

} // namespace Platform::Windows::SdlEnvironment
#endif
