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
// An elevated restart (#2728) hands the launching user's directory over the same way, from its command
// line (adoptHandedOver()), so the Administrator session keeps that user's settings; that one is no test hook.
//
// parse() and resolve() are pure and unit-tested (tests/Core/test_ConfigDirOverride.cpp);
// ConfigDirOverride.cpp holds active(), the only part that reads the environment and touches the disk.

#include "Core/EnvUtils.h"
#include "Platform/AsciiTrim.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Core::ConfigDirOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_CONFIG_DIR";
static_assert(Core::isKnownEnvHook(ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// The directory a TASKSMACK_CONFIG_DIR value (UTF-8, nullptr when unset) names, with surrounding
/// whitespace trimmed; nullopt when it is unset or blank.
[[nodiscard]] inline std::optional<std::filesystem::path> parse(const char* value)
{
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
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

/// The launching user's config directory an elevated restart hands over (--config-dir, #2728): active()
/// returns it, made absolute, from now on, and fromTestHook() is false, as this is no test hook. main.cpp calls
/// it first thing, before anything reads active(). Not thread-safe for the same reason as setActiveForTesting().
void adoptHandedOver(const std::filesystem::path& dir);

/// Whether active() is a directory an elevated restart handed over (adoptHandedOver()): one the less-privileged
/// side chose, so UserConfig writes through no link in it, and nothing read from it is offered to run elevated.
[[nodiscard]] bool handedOver();

/// Whether active() is TASKSMACK_CONFIG_DIR's (a test hook), not a handed-over directory; false when there is
/// no override.
[[nodiscard]] bool fromTestHook();

/// Testing only: active() returns @p dir from now on (nullopt: no override), so each test can have its
/// own config directory (tests/App/ScratchConfigDir.h, #2932). The directory isn't created here, and
/// the environment is left alone. Not thread-safe: call it only while nothing reads active(), with no
/// Core::Application and none of its workers running. Application code never calls it.
void setActiveForTesting(std::optional<std::filesystem::path> dir);

} // namespace Core::ConfigDirOverride
