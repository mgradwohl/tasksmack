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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

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

/// What applyTo() sets in root's place, worked out outside it (the process's, or a test's).
struct RootValues
{
    std::optional<std::string> home;       ///< root's passwd home; HOME is unset when unknown
    std::optional<std::string> runtimeDir; ///< root's own runtime folder (/run/user/0), if it's root's; else unset
    std::optional<std::string> xauthority; ///< root's own copy of the invoking user's Xauthority (applyToThisProcess()); else unset
    /// The PATH to set: applyToThisProcess() gives ROOT_PATH's root-only folders (RootTools::trustedRootPath(), #4666), so
    /// a library that runs a program by name (SDL's zenity file dialog) never finds one in a /usr/local another account
    /// may write, as the children TaskSmack starts haven't since #4396. ROOT_PATH when not given.
    std::string path = std::string(ROOT_PATH);
};

/// The value XDG_DATA_DIRS gets (the XDG default): the user's could list a folder of theirs, where the Vulkan loader
/// finds layers and GSettings schemas (the #4187 review).
inline constexpr std::string_view ROOT_DATA_DIRS = "/usr/local/share:/usr/share";

/// Whether TZ's @p value is kept: a zone name (relative, so under the zoneinfo folder) or a file under
/// /usr/share/zoneinfo, never one with "..": glibc
/// parses the file TZ names (sudo's env_check drops the rest).
[[nodiscard]] bool isSafeTimeZone(std::string_view value) noexcept;

/// Whether @p name is a variable whose value is checked as sudo's env_check does (#4256): LANG, LANGUAGE, LC_*,
/// TERM, COLORTERM, and TaskSmack's TASKSMACK_LOCALE.
[[nodiscard]] bool isLocaleVariable(std::string_view name) noexcept;

/// Whether such a variable's @p value is kept, as sudo's env_check decides (#4256): never one with "/" or "%", nor one
/// starting with "." once leading whitespace is trimmed, which setlocale(), newlocale() or terminfo could take as a path.
[[nodiscard]] bool isSafeLocale(std::string_view value) noexcept;

/// Reduces @p environment ("NAME=value" strings, null-terminated, as environ is) to the allow-list (#4158, #4159,
/// #4179, and the review of #4187): every name not kept, a TZ that isn't safe and a checked variable whose value isn't (isSafeLocale()) are
/// unset (collected first); a relative WAYLAND_DISPLAY is made absolute in the user's runtime folder before XDG_RUNTIME_DIR becomes root's
/// (or is unset); XDG_DATA_DIRS becomes ROOT_DATA_DIRS; XAUTHORITY becomes root's copy of the user's (or is unset); PATH becomes @p root's
/// path (ROOT_PATH's root-only folders, #4666) and HOME root's. Each replaced name is unset first, so no duplicate entry survives. A value
/// that can't be set is unset instead, so nothing of the user's is left by a failure. @return The names removed (a failed unset is left
/// out), in order.
[[nodiscard]] std::vector<std::string> applyTo(const char* const* environment, const RootValues& root, const Operations& ops);

/// The most an Xauthority file may hold to be copied: a few hundred bytes is usual.
inline constexpr long long MOST_XAUTHORITY_BYTES = 1LL << 20;

/// The variables a root session replaces for itself whose user values the programs started as the user get back
/// (SessionUser::userSideValues(), the second #4187 review).
inline constexpr std::array<std::string_view, 3> USER_SIDE_NAMES = {"PATH", "XDG_DATA_DIRS", "XAUTHORITY"};

/// Writes @p contents to a new 0600 file of its own in @p folder (a root-only one), named
/// tasksmack-xauthority-<uid>-<random> (#4667): every session gets its own copy, so a second user's root session can't
/// truncate or rewrite the first's, and no session reads another's cookies. A partly written file is removed.
/// @return The copy's path, or nullopt when it can't be made or written whole.
[[nodiscard]] std::optional<std::string> writeXauthorityCopy(const std::string& folder, uid_t uid, const std::string& contents);

/// Removes @p copy when TaskSmack exits normally (std::atexit()), as the session's copy is no use after it (#4667). A
/// later call replaces the path; a crash leaves it until /run is cleared at the next boot. A root program TaskSmack started
/// keeps the X connections it has; one it opens after TaskSmack exits finds no cookie.
void removeAtExit(const std::string& copy);

/// Opens @p path read-only as @p uid would (#4187): setfsuid/setfsgid to them and their primary group for the open,
/// then back to the effective ids, without following a final link (O_NOFOLLOW) or blocking on a FIFO
/// (O_NONBLOCK). -1 when it can't; TaskSmack stops if the ids can't be put back. Not thread-safe (setfsuid).
[[nodiscard]] int openAsUser(const std::string& path, uid_t uid);

/// @p fd's whole contents when it is a regular file @p owner owns of at most @p mostBytes (#4255), else nullopt.
[[nodiscard]] std::optional<std::string> readOwnedFile(int fd, uid_t owner, long long mostBytes);

/// Whether @p folder is a folder (not a link) @p owner owns that no group or other may open (0700 or tighter).
[[nodiscard]] bool isPrivateFolderOf(const std::string& folder, uid_t owner);

/// Each of USER_SIDE_NAMES' values from @p env that is set, and XAUTHORITY as nullopt when it isn't (#4259): a user who had
/// none gets none, never root's private copy. An unset PATH or XDG_DATA_DIRS is left out, so the child gets the process's.
[[nodiscard]] std::map<std::string, std::optional<std::string>, std::less<>>
userSideValuesFrom(const std::function<std::optional<std::string>(std::string_view)>& env);

/// main()'s call, first thing, before any thread or SDL call: when @p apply, applyTo() this process's environment with
/// PATH ROOT_PATH's root-only folders (RootTools::trustedRootPath(); a folder in /proc when none is, #4666), then checks
/// it: a PATH that isn't that one ends TaskSmack (fail closed). Not thread-safe (setenv, unsetenv).
void applyToThisProcess(bool apply);

/// The names applyToThisProcess() removed, for runApp() to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

} // namespace Platform::RootEnvironment

#endif
