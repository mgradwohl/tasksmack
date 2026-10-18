#pragma once

/// @file ConfigDirectory.h
/// @brief Where TaskSmack's per-user config directory is on Linux and other XDG systems (#1891), in one
/// place: UserConfig puts config.toml there and LinuxPathProvider gives it to PathService for the user
/// themes, which must agree (#2438). Purely lexical; the callers read the environment.

#include <algorithm>
#include <filesystem>
#include <functional>
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
            return {.directory = fallback,
                    .warning = "Ignoring unsafe config directory " + candidate.string() + "; using " + fallback.string()};
        }
        return {.directory = candidate, .warning = {}};
    }
    return {.directory = fallback, .warning = {}};
}

} // namespace Platform::ConfigDirectory
