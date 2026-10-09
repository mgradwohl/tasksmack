#pragma once

// A test-only config directory (#1596): give a test launch its own config.toml and single-instance
// lock, so it neither shares the user's settings nor is refused because the user's TaskSmack runs.
//
//   TASKSMACK_CONFIG_DIR=/tmp/tasksmack-test ./TaskSmack
//
// When set and not blank, the directory replaces the platform config directory
// (%APPDATA%\TaskSmack on Windows, $XDG_CONFIG_HOME/tasksmack or ~/.config/tasksmack on Linux) for
// everything stored there: config.toml (UserConfig), the instance lock beside it (main.cpp) and the
// user themes folder (PathService::userConfigDir()). A relative path is taken from the working
// directory at startup, and the directory is created if missing. Unset or blank changes nothing.
//
// parse() and resolve() are pure and unit-tested (tests/Core/test_ConfigDirOverride.cpp);
// ConfigDirOverride.cpp holds active(), the only part that reads the environment and touches the disk.

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Core::ConfigDirOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_CONFIG_DIR";

/// The directory a TASKSMACK_CONFIG_DIR value (UTF-8, nullptr when unset) names, with surrounding
/// whitespace trimmed; nullopt when it is unset or blank.
[[nodiscard]] inline std::optional<std::filesystem::path> parse(const char* value)
{
    std::string_view text = value != nullptr ? value : "";
    const auto isSpace = [](const char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && isSpace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back()))
    {
        text.remove_suffix(1);
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// The config directory to use: the override when there is one, else @p platformDir.
[[nodiscard]] inline std::filesystem::path resolve(const std::optional<std::filesystem::path>& override,
                                                   const std::filesystem::path& platformDir)
{
    return override.value_or(platformDir);
}

/// The overriding directory, absolute; nullopt when TASKSMACK_CONFIG_DIR is unset or blank. Read on
/// the first call, which also creates the directory (a failure is logged as a warning). Thread-safe.
[[nodiscard]] const std::optional<std::filesystem::path>& active();

} // namespace Core::ConfigDirOverride
