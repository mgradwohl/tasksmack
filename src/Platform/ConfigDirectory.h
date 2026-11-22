#pragma once

/// @file ConfigDirectory.h
/// @brief Where TaskSmack's per-user config directory is on Linux and other XDG systems (#1891), in one
/// place: UserConfig puts config.toml there and LinuxPathProvider gives it to PathService for the user
/// themes, which must agree (#2438). Purely lexical; the callers read the environment.

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>

namespace Platform::ConfigDirectory
{

/// Validates a candidate config directory path derived from user/environment input.
/// Returns true if the path is safe to use as a config directory:
///   - must be absolute after lexical normalization
///   - must not contain any ".." traversal components after normalization
///
/// This is a purely lexical check (no filesystem access). Callers that need
/// symlink-safe validation should resolve the path with weakly_canonical() first
/// and then call this function on the result.
[[nodiscard]] inline auto isValidConfigDir(const std::filesystem::path& candidate) -> bool
{
    const auto normalized = candidate.lexically_normal();
    if (!normalized.is_absolute())
    {
        return false;
    }
    return !std::ranges::any_of(normalized, [](const auto& part) { return part == ".."; });
}

/// Where the config lives on Linux and other XDG systems, and why a candidate was passed over:
/// configDirectoryFrom()'s answer. `warning` is empty when nothing was.
struct ConfigDirectoryChoice
{
    std::filesystem::path directory;
    std::string warning;
};

/// The XDG config directory (#1891): $XDG_CONFIG_HOME/tasksmack, else the home directory's
/// .config/tasksmack -- from $HOME, else @p passwdHome (the passwd entry's directory, asked only when
/// $HOME is unset), else @p currentPath. @p xdgConfigHome and @p home are the variables' values, nullopt
/// when unset or empty. An XDG directory that isn't absolute or climbs with ".." (isValidConfigDir())
/// is passed over for the home one; a home one that isn't absolute, for @p currentPath. Purely lexical:
/// the caller reads the environment and logs the warning.
[[nodiscard]] inline auto configDirectoryFrom(const std::optional<std::string>& xdgConfigHome,
                                              const std::optional<std::string>& home,
                                              const std::function<std::optional<std::filesystem::path>()>& passwdHome,
                                              const std::filesystem::path& currentPath) -> ConfigDirectoryChoice
{
    std::filesystem::path fallback = currentPath;
    if (home.has_value())
    {
        fallback = std::filesystem::path(*home) / ".config" / "tasksmack";
    }
    else if (const std::optional<std::filesystem::path> passwd = passwdHome ? passwdHome() : std::nullopt; passwd.has_value())
    {
        fallback = *passwd / ".config" / "tasksmack";
    }
    if (!fallback.is_absolute())
    {
        return {.directory = currentPath, .warning = "Fallback config directory is not absolute: " + fallback.string()};
    }
    if (xdgConfigHome.has_value())
    {
        const std::filesystem::path candidate = (std::filesystem::path(*xdgConfigHome) / "tasksmack").lexically_normal();
        if (!isValidConfigDir(candidate))
        {
            return {
                .directory = fallback,
                .warning = "Ignoring unsafe config directory " + candidate.string() + "; using " + fallback.string(),
            };
        }
        return {.directory = candidate, .warning = {}};
    }
    return {.directory = fallback, .warning = {}};
}

/// Whether @p dir, lexically normalized, is @p home or inside it, component by component ("/home/u2" isn't inside
/// "/home/u"). Purely lexical, as the rest of this file.
[[nodiscard]] inline auto isInside(const std::filesystem::path& dir, const std::filesystem::path& home) -> bool
{
    const std::filesystem::path normalDir = dir.lexically_normal();
    const std::filesystem::path normalHome = home.lexically_normal();
    auto homePart = normalHome.begin();
    auto dirPart = normalDir.begin();
    for (; homePart != normalHome.end(); ++homePart, ++dirPart)
    {
        if (homePart->empty() && std::next(homePart) == normalHome.end())
        {
            break; // a trailing separator ("/home/u/") ends the home
        }
        if (dirPart == normalDir.end() || *dirPart != *homePart)
        {
            return false;
        }
    }
    return true;
}

/// configDirectoryFrom() for a TaskSmack run as root for a user (#3580), or in a capabilities session (#4802):
/// @p invokingUserHome is the home directory of the user sudo or pkexec names, or a capabilities session's own user's
/// from the passwd entry (SessionUser::settingsOwnerHome()), nullopt otherwise. Both read past the user's file
/// permissions with the user's environment, so the environment names settings only inside that home. Such a session keeps that user's
/// settings, not root's. When $HOME is theirs (sudo -E) their XDG_CONFIG_HOME is used only inside their home (#4153):
/// root writes the config, the lock and the themes there, so a folder anywhere else (/etc) is passed over, with a
/// warning, for their home's .config/tasksmack. Otherwise (pkexec, and sudo, set HOME to root's) it's their home's
/// .config/tasksmack, and the environment's XDG_CONFIG_HOME, root's, is ignored. @p rootWithAnotherUsersEnvironment: root,
/// and SessionUser::thisProcessMayHaveAnotherUsersEnvironment() (#4172).
[[nodiscard]] inline auto configDirectoryFor(const std::optional<std::string>& xdgConfigHome,
                                             const std::optional<std::string>& home,
                                             const std::function<std::optional<std::filesystem::path>()>& passwdHome,
                                             const std::filesystem::path& currentPath,
                                             const std::optional<std::string>& invokingUserHome,
                                             bool rootWithAnotherUsersEnvironment = false) -> ConfigDirectoryChoice
{
    // Root whose environment may be a user's, with no known user to keep the settings of (su, doas keepenv: no SUDO_UID,
    // #4172): root's own folder, from its passwd entry, and neither XDG_CONFIG_HOME nor HOME, which are the user's.
    if (rootWithAnotherUsersEnvironment && !invokingUserHome.has_value())
    {
        // Never the working directory, which su keeps as the user's: root's passwd home, or /root when it can't be read
        // (the #4182 review).
        const std::optional<std::filesystem::path> rootHome = passwdHome ? passwdHome() : std::nullopt;
        ConfigDirectoryChoice choice{
            .directory =
                (rootHome.has_value() && rootHome->is_absolute() ? *rootHome : std::filesystem::path("/root")) / ".config" / "tasksmack",
            .warning = {},
        };
        // Said only when the environment would have chosen another folder (plain su sets HOME to root's anyway).
        if (const std::filesystem::path environments = configDirectoryFrom(xdgConfigHome, home, passwdHome, currentPath).directory;
            (xdgConfigHome.has_value() || home.has_value()) && environments.lexically_normal() != choice.directory.lexically_normal())
        {
            choice.warning = "Ignoring XDG_CONFIG_HOME and HOME: TaskSmack runs as root with an environment that may be "
                             "another user's; using " +
                             choice.directory.string();
        }
        return choice;
    }
    if (invokingUserHome.has_value() && home != invokingUserHome)
    {
        return configDirectoryFrom(std::nullopt, invokingUserHome, passwdHome, currentPath);
    }
    // A home of "/" (some service accounts) would hold every path, so it vouches for none (#4155 review).
    if (invokingUserHome.has_value() && xdgConfigHome.has_value() &&
        (std::filesystem::path(*invokingUserHome).lexically_normal() == "/" || !isInside(*xdgConfigHome, *invokingUserHome)))
    {
        ConfigDirectoryChoice choice = configDirectoryFrom(std::nullopt, invokingUserHome, passwdHome, currentPath);
        choice.warning = "Ignoring XDG_CONFIG_HOME " + *xdgConfigHome +
                         ": TaskSmack runs with more access than its user (as root for them, or with file capabilities), "
                         "and keeps their settings only inside their home; using " +
                         choice.directory.string();
        return choice;
    }
    return configDirectoryFrom(xdgConfigHome, home, passwdHome, currentPath);
}

} // namespace Platform::ConfigDirectory
