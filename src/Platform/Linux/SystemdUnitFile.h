#pragma once

// The parts of a systemd unit file the Startup tab shows (#3082): what a user unit runs at sign-in, or what
// a timer starts and when. Pure: the text is the caller's, so it is tested without a filesystem.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::SystemdUnitFile
{

/// What a .service or .timer file says, as far as the Startup tab needs it.
struct Unit
{
    std::string description; ///< [Unit] Description=
    std::string execStart;   ///< [Service] ExecStart=, the first one, without its -, @, +, ! or : prefixes
    std::string schedule;    ///< [Timer] the first OnCalendar=, OnBootSec=, OnStartupSec= or OnUnitActiveSec=, as "OnCalendar=daily"
    std::string timerUnit;   ///< [Timer] Unit= (empty: the timer's own name with .service)
    /// [Install] WantedBy=, every target named (#3425): what `systemctl --user enable` links it into. An empty value
    /// resets the list, as systemd reads it.
    std::vector<std::string> wantedBy;
};

/// The targets a user unit starts with the session or on a timer from (#3082, #3425): enabled into one of these, it's
/// a startup entry.
inline constexpr std::array<std::string_view, 3> SESSION_TARGETS = {"default.target", "graphical-session.target", "timers.target"};

/// Whether @p unit's [Install] section would enable it into one of SESSION_TARGETS.
[[nodiscard]] inline bool installsIntoSession(const Unit& unit)
{
    return std::ranges::any_of(
        unit.wantedBy, [](const std::string& target) { return std::ranges::find(SESSION_TARGETS, target) != SESSION_TARGETS.end(); });
}

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

/// @p text parsed: keys outside the four sections, comments (# and ;) and continuation lines are ignored.
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
        else if (section == "Install" && key == "WantedBy")
        {
            if (value.empty())
            {
                unit.wantedBy.clear();
            }
            while (!value.empty())
            {
                const std::size_t space = value.find_first_of(" \t");
                const std::string_view target = value.substr(0, space);
                if (!target.empty())
                {
                    unit.wantedBy.emplace_back(target);
                }
                value = space == std::string_view::npos ? std::string_view{} : Detail::trim(value.substr(space));
            }
        }
    }
    return unit;
}

} // namespace Platform::SystemdUnitFile
