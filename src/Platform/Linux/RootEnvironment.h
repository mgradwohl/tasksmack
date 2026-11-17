#pragma once

// A Linux root session's environment, when it may be a less-privileged user's (#4158, #4159, #4179; the maintainer's
// decision of 2026-11-17): only an allow-list of variables is kept, as sudo's env_reset keeps them, and PATH and HOME
// are root's own. A list of bad names kept missing some (SDL's, then the GL loaders', then GTK's through libdecor's
// plugin, PATH choosing zenity, PYTHONPATH reaching gdb): an allow-list doesn't.
//
// Kept: how to reach the user's display and their locale and desktop (what SessionUser::KEPT_VARIABLES rebuilds a
// user-side child from, so links still open in their session), the terminal, what names the user who started it
// (sudo, pkexec, doas), and TaskSmack's own TASKSMACK_* hooks (the writing ones are ignored by the gate, #4104).
// Everything else goes, logged by name, never value. Cost: an env-chosen driver or tool (PRIME offload's
// __GLX_VENDOR_LIBRARY_NAME, a VK ICD) no longer applies in such a session.
//
// The logic takes the environment and its operations as arguments, so test_RootEnvironment.cpp covers it with a fake
// one; applyToThisProcess() works on the process's own.

#ifdef __linux__

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::RootEnvironment
{

/// The variables kept by name.
inline constexpr std::array<std::string_view, 21> KEPT = {
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XAUTHORITY",
    "XDG_RUNTIME_DIR",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "XDG_DATA_DIRS",
    "DESKTOP_SESSION",
    "LANG",
    "LANGUAGE",
    "TZ",
    "TERM",
    "COLORTERM",
    "SUDO_UID",
    "SUDO_GID",
    "SUDO_USER",
    "SUDO_COMMAND",
    "PKEXEC_UID",
    "DOAS_USER",
    "PATH",
    "HOME",
};

/// The prefixes whose variables are kept: the locale's categories, and TaskSmack's own hooks.
inline constexpr std::array<std::string_view, 2> KEPT_PREFIXES = {"LC_", "TASKSMACK_"};

/// root's PATH in such a session, whatever the user's was (#4159): sudo's secure_path, on any distribution.
inline constexpr std::string_view ROOT_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

/// Whether @p name is kept (KEPT, or a KEPT_PREFIXES prefix). Exact case.
[[nodiscard]] bool isKept(std::string_view name) noexcept;

/// What applyTo() does to an environment: the process's own, or a test's.
struct Operations
{
    std::function<bool(const std::string&)> unset;
    std::function<bool(const std::string&, const std::string&)> set;
};

/// Reduces @p environment ("NAME=value" strings, null-terminated, as environ is) to the allow-list: every other name
/// is unset (collected first), PATH is set to ROOT_PATH, and HOME to @p rootHome (or unset when unknown).
/// @return The names removed (a failed unset is left out), in order.
[[nodiscard]] std::vector<std::string>
applyTo(const char* const* environment, const std::optional<std::string>& rootHome, const Operations& ops);

/// main()'s call, first thing, before any thread or SDL call: when @p apply, applyTo() this process's environment.
/// Not thread-safe (setenv, unsetenv).
void applyToThisProcess(bool apply);

/// The names applyToThisProcess() removed, for runApp() to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

} // namespace Platform::RootEnvironment

#endif
