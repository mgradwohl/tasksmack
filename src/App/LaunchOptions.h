#pragma once

// TaskSmack's command-line arguments (#3093, part 2): --minimized, which the sign-in entry passes
// (Domain::StartAtLogin::runCommand()), starts it minimized without taking the focus. Anything else is
// ignored, and listed for the log. Header-only and pure, so main.cpp's parsing is tested on every host.
//
// Started in place of Task Manager (#3072), Windows passes taskmgr.exe's path as the first argument and
// Task Manager's own arguments after it (TaskManagerLaunch.h). Those are Task Manager's, not TaskSmack's:
// the start is marked asTaskManager and none of them is read as TaskSmack's, or listed as ignored.
//
// Restarted as administrator (#2728, Platform/IElevatedRestart.h), TaskSmack is given "--handoff <PID>
// --config-dir <directory>": the Standard TaskSmack to wait for, and the launching user's config directory.
// The directory is taken only with a hand-off, so --config-dir alone is ignored, as is a hand-off whose PID
// isn't a whole number.

#include "App/TaskManagerLaunch.h"
#include "Domain/StartAtLogin.h"
#include "Platform/IElevatedRestart.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{

/// What the command line asked for.
struct LaunchOptions
{
    std::vector<std::string> ignored;        ///< Arguments TaskSmack doesn't take, for the log
    bool minimized = false;                  ///< --minimized: start minimized, without taking the focus
    bool asTaskManager = false;              ///< Windows started TaskSmack in place of Task Manager (#3072)
    std::optional<std::uint32_t> handoffPid; ///< --handoff: the Standard TaskSmack this elevated one replaces (#2728)
    std::optional<std::string> configDir;    ///< --config-dir, with a hand-off: the launching user's (UTF-8)
};

namespace Detail
{
/// @p text as a PID: a whole, non-zero number, nothing else.
[[nodiscard]] inline std::optional<std::uint32_t> parsePid(const std::string& text) noexcept
{
    std::uint32_t pid = 0;
    const char* const end = std::to_address(text.end());
    const auto [last, error] = std::from_chars(std::to_address(text.begin()), end, pid);
    if (error != std::errc{} || last != end || pid == 0)
    {
        return std::nullopt;
    }
    return pid;
}
} // namespace Detail

/// Reads @p arguments, the program's own path first (argv[0]), which is skipped.
[[nodiscard]] inline LaunchOptions parseLaunchOptions(std::span<const std::string> arguments)
{
    LaunchOptions options;
    const std::span<const std::string> own = arguments.subspan(arguments.empty() ? 0 : 1);
    if (TaskManagerLaunch::isTaskManagerLaunch(own))
    {
        options.asTaskManager = true; // taskmgr.exe and its arguments: none of them is TaskSmack's
        return options;
    }
    std::optional<std::string> configDir;
    for (std::size_t index = 0; index < own.size(); ++index)
    {
        const std::string& argument = own[index];
        const bool hasValue = index + 1 < own.size();
        if (argument == Domain::StartAtLogin::MINIMIZED_ARGUMENT)
        {
            options.minimized = true;
        }
        else if (argument == Platform::HANDOFF_ARGUMENT && hasValue && Detail::parsePid(own[index + 1]).has_value())
        {
            options.handoffPid = Detail::parsePid(own[++index]);
        }
        else if (argument == Platform::CONFIG_DIR_ARGUMENT && hasValue && !own[index + 1].empty())
        {
            configDir = own[++index];
        }
        else
        {
            options.ignored.push_back(argument);
        }
    }
    if (configDir.has_value())
    {
        if (options.handoffPid.has_value())
        {
            options.configDir = std::move(configDir);
        }
        else
        {
            options.ignored.emplace_back(Platform::CONFIG_DIR_ARGUMENT);
        }
    }
    return options;
}

} // namespace App
