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
#include "Platform/IHandedOverFolder.h"

#include <cctype>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
/// returns it, made absolute, from now on, and fromTestHook() is false, as this is no test hook. Kept apart from
/// TASKSMACK_CONFIG_DIR's (#3780): a hand-off session never reads that variable, nor creates its directory.
/// @p folder is the folder as checked and held (Platform::checkHandedOverConfigDir()), which main.cpp owns;
/// only a weak reference is kept here (handedOverFolder()). main.cpp calls this first thing, before anything
/// reads active(). Not thread-safe for the same reason as setActiveForTesting().
void adoptHandedOver(const std::filesystem::path& dir, std::weak_ptr<const Platform::IHandedOverFolder> folder = {});

/// A TaskSmack run as root for a user (sudo or pkexec, #3580) keeps that user's config directory, the less-privileged
/// side's, as an elevated restart's handed-over one is (#3759): main.cpp calls this first thing on Linux when
/// SessionUser::invokingUserHome() names a user, and handedOver() is true from then on. Not thread-safe, as above.
void markLessPrivilegedFolder();

/// Whether the config directory is the less-privileged side's: one an elevated restart handed over (adoptHandedOver()),
/// or, run as root for a user, that user's (markLessPrivilegedFolder()). UserConfig then writes through no link in it,
/// and nothing read from it (a dump folder, recent commands) is offered to run elevated.
[[nodiscard]] bool handedOver();

/// The held folder adoptHandedOver() was given, while its owner (main.cpp) still holds it; null when nothing was
/// handed over, or it is no longer held.
[[nodiscard]] std::shared_ptr<const Platform::IHandedOverFolder> handedOverFolder();

/// A file read from the held handed-over folder (readHandedOverFiles()).
struct HandedOverFile
{
    std::filesystem::path path; ///< Its full path, for what is logged and shown
    std::string text;
};

/// The plain files in folder @p folder of the held handed-over folder whose names end in @p extension, each listed
/// and read through it (#3774), relative to the folder held: the user themes, for the UI, which uses no Platform
/// type. nullopt when no handed-over folder is held; an error when the folder can't be listed. A file that can't be
/// read (a link, a hard link) is skipped, and logged.
[[nodiscard]] std::optional<std::expected<std::vector<HandedOverFile>, std::string>>
readHandedOverFiles(const std::filesystem::path& folder, std::string_view extension);

/// Whether an elevated restart handed a folder over (adoptHandedOver()) that is no longer held, or was never given
/// one: such a hand-off fails closed, and UserConfig neither reads nor saves there. False with no hand-off, and in a
/// root session for a user (markLessPrivilegedFolder()), which holds no folder.
[[nodiscard]] bool handedOverFolderReleased();

/// Whether active() is TASKSMACK_CONFIG_DIR's (a test hook), not a handed-over directory; false when there is
/// no override.
[[nodiscard]] bool fromTestHook();

/// Testing only: active() returns @p dir from now on (nullopt: no override), so each test can have its
/// own config directory (tests/App/ScratchConfigDir.h, #2932). The directory isn't created here, and
/// the environment is left alone. Not thread-safe: call it only while nothing reads active(), with no
/// Core::Application and none of its workers running. Application code never calls it.
void setActiveForTesting(std::optional<std::filesystem::path> dir);

} // namespace Core::ConfigDirOverride
