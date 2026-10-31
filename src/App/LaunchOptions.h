#pragma once

// TaskSmack's command-line arguments (#3093, part 2): --minimized, which the sign-in entry passes
// (Domain::StartAtLogin::runCommand()), starts it minimized without taking the focus. Anything else is
// ignored, and listed for the log. Header-only and pure, so main.cpp's parsing is tested on every host.

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
};

/// Reads @p arguments, the program's own path first (argv[0]), which is skipped.
[[nodiscard]] inline LaunchOptions parseLaunchOptions(std::span<const std::string> arguments)
{
    LaunchOptions options;
    for (const std::string& argument : arguments.subspan(arguments.empty() ? 0 : 1))
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
