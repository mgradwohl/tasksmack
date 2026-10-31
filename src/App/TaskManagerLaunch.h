#pragma once

// TaskSmack started in place of Task Manager (#3072). With taskmgr.exe's IFEO Debugger pointing at
// TaskSmack, Windows starts
//   "C:\Program Files\TaskSmack\TaskSmack.exe" C:\WINDOWS\system32\taskmgr.exe [Task Manager's arguments]
// so TaskSmack's arguments begin with Task Manager's own path, then whatever Task Manager was asked for
// (/4, /7 /startup, ...). TaskSmack takes no arguments of its own: these are recognised so the start can
// be told apart (logged, and a second instance hands the foreground to the first instead of a message
// box), and are otherwise ignored. Pure, so it is tested on every platform.

#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <span>
#include <string>
#include <string_view>

namespace App::TaskManagerLaunch
{

/// Whether @p argument names Task Manager's executable: its file name, after any folder and without
/// surrounding quotes or blanks, is taskmgr.exe or taskmgr (ASCII letters ignoring case).
[[nodiscard]] constexpr bool namesTaskManager(std::string_view argument) noexcept
{
    argument = Platform::trimAscii(argument);
    if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"')
    {
        argument.remove_prefix(1);
        argument.remove_suffix(1);
    }
    const std::size_t slash = argument.find_last_of("\\/");
    if (slash != std::string_view::npos)
    {
        argument.remove_prefix(slash + 1);
    }
    return Platform::equalsIgnoreCase(argument, "taskmgr.exe") || Platform::equalsIgnoreCase(argument, "taskmgr");
}

/// Whether @p arguments (the command line after the program's name) are Windows starting TaskSmack in
/// place of Task Manager: the first one names Task Manager. Whatever follows is Task Manager's, and is
/// ignored.
[[nodiscard]] constexpr bool isTaskManagerLaunch(std::span<const std::string> arguments) noexcept
{
    return !arguments.empty() && namesTaskManager(arguments.front());
}

} // namespace App::TaskManagerLaunch
