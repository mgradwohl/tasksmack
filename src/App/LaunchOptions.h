#pragma once

// TaskSmack's command-line arguments (#3093, part 2): --minimized, which the sign-in entry passes
// (Domain::StartAtLogin::runCommand()), starts it minimized without taking the focus. Anything else is
// ignored, and listed for the log. Header-only and pure, so main.cpp's parsing is tested on every host.
//
// Started in place of Task Manager (#3072), Windows passes taskmgr.exe's path as the first argument and
// Task Manager's own arguments after it (TaskManagerLaunch.h). Those are Task Manager's, not TaskSmack's:
// the start is marked asTaskManager and none of them is read as TaskSmack's, or listed as ignored.
//
// Restarted as administrator (#2728, Platform/IElevatedRestart.h), TaskSmack is given "--handoff <PID> <start
// ticks> --config-dir <directory>": the Standard TaskSmack to wait for, its creation time, and the launching user's
// config directory. The directory is taken only with a hand-off, so --config-dir alone is ignored, as is a hand-off
// whose PID or creation time isn't a whole, non-zero number. main.cpp still checks the directory before it uses it
// (Platform::checkHandedOverConfigDir()): the command line is the less-privileged side's.

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
    std::uint64_t handoffStartTicks = 0;     ///< --handoff's second value: that process's creation time
    std::optional<std::string> configDir;    ///< --config-dir, with a hand-off: the launching user's (UTF-8)
};

namespace Detail
{
/// @p text as a whole, non-zero number of type @p Number, nothing else.
template<typename Number> [[nodiscard]] std::optional<Number> parsePositive(const std::string& text) noexcept
{
    Number value = 0;
    const char* const end = std::to_address(text.end());
    const auto [last, error] = std::from_chars(std::to_address(text.begin()), end, value);
    if (error != std::errc{} || last != end || value == 0)
    {
        return std::nullopt;
    }
    return value;
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
    // Each argument and the values it takes are consumed from the front of what is left.
    for (std::span<const std::string> rest = own; !rest.empty();)
    {
        const std::string& argument = rest.front();
        std::size_t used = 1;
        if (argument == Domain::StartAtLogin::MINIMIZED_ARGUMENT)
        {
            options.minimized = true;
        }
        else if (argument == Platform::HANDOFF_ARGUMENT && rest.size() >= 3 && Detail::parsePositive<std::uint32_t>(rest[1]).has_value() &&
                 Detail::parsePositive<std::uint64_t>(rest[2]).has_value())
        {
            options.handoffPid = Detail::parsePositive<std::uint32_t>(rest[1]);
            options.handoffStartTicks = Detail::parsePositive<std::uint64_t>(rest[2]).value_or(0);
            used = 3;
        }
        else if (argument == Platform::CONFIG_DIR_ARGUMENT && rest.size() >= 2 && !rest[1].empty())
        {
            configDir = rest[1];
            used = 2;
        }
        else
        {
            options.ignored.push_back(argument);
        }
        rest = rest.subspan(used);
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
