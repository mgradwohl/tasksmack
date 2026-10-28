#pragma once

// The parts of a systemd unit file the Startup tab shows (#3082): what a user unit runs at sign-in, or what
// a timer starts and when. Pure: the text is the caller's, so it is tested without a filesystem.

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform::SystemdUnitFile
{

/// What a .service or .timer file says, as far as the Startup tab needs it.
struct Unit
{
    std::string description; ///< [Unit] Description=
    std::string execStart;   ///< [Service] ExecStart=, the first one, without its -, @, +, ! or : prefixes
    std::string schedule;    ///< [Timer] the first OnCalendar=, OnBootSec=, OnStartupSec= or OnUnitActiveSec=, as "OnCalendar=daily"
    std::string timerUnit;   ///< [Timer] Unit= (empty: the timer's own name with .service)
};

namespace Detail
{

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace Detail

/// @p text parsed: keys outside the three sections, comments (# and ;) and continuation lines are ignored.
[[nodiscard]] inline Unit parse(std::string_view text)
{
    Unit unit;
    std::string_view section;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = Detail::trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';')
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = Detail::trim(line.substr(0, equals));
        std::string_view value = Detail::trim(line.substr(equals + 1));
        if (section == "Unit" && key == "Description" && unit.description.empty())
        {
            unit.description = value;
        }
        else if (section == "Service" && key == "ExecStart" && unit.execStart.empty() && !value.empty())
        {
            while (!value.empty() && std::string_view("-@+!:").contains(value.front()))
            {
                value.remove_prefix(1);
            }
            unit.execStart = value;
        }
        else if (section == "Timer" && unit.schedule.empty() &&
                 (key == "OnCalendar" || key == "OnBootSec" || key == "OnStartupSec" || key == "OnUnitActiveSec"))
        {
            unit.schedule = std::string(key) + "=" + std::string(value);
        }
        else if (section == "Timer" && key == "Unit")
        {
            unit.timerUnit = value;
        }
    }
    return unit;
}

} // namespace Platform::SystemdUnitFile
