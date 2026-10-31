#pragma once

// TaskSmack's command-line arguments (#3093, part 2): --minimized, which the sign-in entry passes
// (Domain::StartAtLogin::runCommand()), starts it minimized without taking the focus. Anything else is
// ignored, and listed for the log. Header-only and pure, so main.cpp's parsing is tested on every host.
//
// Started in place of Task Manager (#3072), Windows passes taskmgr.exe's path as the first argument and
// Task Manager's own arguments after it (TaskManagerLaunch.h). Those are Task Manager's, not TaskSmack's:
// the start is marked asTaskManager and none of them is read as TaskSmack's, or listed as ignored.

#include "App/TaskManagerLaunch.h"
#include "Domain/StartAtLogin.h"

#include <span>
#include <string>
#include <vector>

namespace App
{

/// What the command line asked for.
struct LaunchOptions
{
    std::vector<std::string> ignored; ///< Arguments TaskSmack doesn't take, for the log
    bool minimized = false;           ///< --minimized: start minimized, without taking the focus
    bool asTaskManager = false;       ///< Windows started TaskSmack in place of Task Manager (#3072)
};

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
    for (const std::string& argument : own)
    {
        if (argument == Domain::StartAtLogin::MINIMIZED_ARGUMENT)
        {
            options.minimized = true;
        }
        else
        {
            options.ignored.push_back(argument);
        }
    }
    return options;
}

} // namespace App
