#pragma once

// Run new task's recent commands (#3065): the last few command lines run, newest first, kept in
// config.toml ([run_task] recent) and offered in the dialog. Pure, so tested without ImGui.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace App::RunTaskHistory
{

/// How many commands are remembered.
inline constexpr std::size_t MAX_RECENT = 10;

/// A command longer than this isn't remembered or loaded (a hand-edited config can't grow without bound).
inline constexpr std::size_t MAX_COMMAND_BYTES = 4096;

/// @p recent with @p command first, any earlier copy of it removed, and no more than MAX_RECENT kept. A
/// blank or over-long command leaves the list as it was.
[[nodiscard]] inline std::vector<std::string> remember(std::vector<std::string> recent, std::string_view command)
{
    if (command.find_first_not_of(" \t\n") == std::string_view::npos || command.size() > MAX_COMMAND_BYTES)
    {
        return recent;
    }
    std::erase(recent, command);
    recent.insert(recent.begin(), std::string(command));
    if (recent.size() > MAX_RECENT)
    {
        recent.resize(MAX_RECENT);
    }
    return recent;
}

/// config.toml's list as the dialog takes it: blank, over-long and repeated commands left out (the first
/// copy kept, so the order stays newest first), and no more than MAX_RECENT.
[[nodiscard]] inline std::vector<std::string> sanitize(const std::vector<std::string>& stored)
{
    std::vector<std::string> recent;
    for (const std::string& command : stored)
    {
        if (recent.size() >= MAX_RECENT)
        {
            break;
        }
        if (command.find_first_not_of(" \t\n") != std::string::npos && command.size() <= MAX_COMMAND_BYTES &&
            std::ranges::find(recent, command) == recent.end())
        {
            recent.push_back(command);
        }
    }
    return recent;
}

} // namespace App::RunTaskHistory
