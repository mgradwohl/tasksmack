#pragma once

/// @file CronTab.h
/// @brief Reading cron tables for the Linux Startup tab (#3425): the user's crontab (`crontab -l`) and the system's
/// (/etc/crontab, /etc/cron.d/*), which add a user column. Pure text parsing; no I/O.

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::CronTab
{

/// One job line of a cron table.
struct Entry
{
    std::string schedule; ///< "@reboot", "@daily", ... or the five fields as written ("*/5 * * * *")
    std::string user;     ///< The user it runs as: the system tables' sixth column; empty in a user's own crontab
    std::string command;  ///< The rest of the line, as cron hands it to the shell
    std::size_t line = 0; ///< 1-based line number in its table, which identifies it there
};

namespace Detail
{

[[nodiscard]] constexpr bool isBlank(char c) noexcept
{
    return c == ' ' || c == '\t';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (isBlank(text.front()) || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (isBlank(text.back()) || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

/// The next whitespace-separated word of @p text, removed from it; empty at the end.
[[nodiscard]] constexpr std::string_view nextWord(std::string_view& text)
{
    while (!text.empty() && isBlank(text.front()))
    {
        text.remove_prefix(1);
    }
    std::size_t end = 0;
    while (end < text.size() && !isBlank(text[end]))
    {
        ++end;
    }
    const std::string_view word = text.substr(0, end);
    text.remove_prefix(end);
    return word;
}

/// Whether @p line sets an environment variable ("MAILTO=x", "PATH = /bin") rather than naming a job: a name of
/// letters, digits and underscores, then '='.
[[nodiscard]] constexpr bool isAssignment(std::string_view line) noexcept
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == '_' || (line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
                               (i > 0 && line[i] >= '0' && line[i] <= '9')))
    {
        ++i;
    }
    if (i == 0)
    {
        return false;
    }
    while (i < line.size() && isBlank(line[i]))
    {
        ++i;
    }
    return i < line.size() && line[i] == '=';
}

/// Whether @p word is one of cron's named schedules.
[[nodiscard]] constexpr bool isNamedSchedule(std::string_view word) noexcept
{
    constexpr std::array<std::string_view, 8> NAMES = {
        "@reboot",
        "@yearly",
        "@annually",
        "@monthly",
        "@weekly",
        "@daily",
        "@midnight",
        "@hourly",
    };
    return std::ranges::any_of(NAMES, [word](std::string_view name) { return word == name; });
}

} // namespace Detail

/// The job lines of a cron table. @p withUser: the system tables' format, with the user column after the schedule.
/// Comments, blank lines and variable assignments are skipped, and so is a line too short to be a job.
[[nodiscard]] inline std::vector<Entry> parse(std::string_view text, bool withUser)
{
    std::vector<Entry> entries;
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        ++lineNumber;
        const std::string_view line = Detail::trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#' || Detail::isAssignment(line))
        {
            continue; // past the last line, start > text.size() ends the loop
        }
        std::string_view rest = line;
        std::string schedule;
        const std::string_view first = Detail::nextWord(rest);
        if (first.starts_with('@'))
        {
            if (!Detail::isNamedSchedule(first))
            {
                continue;
            }
            schedule = std::string(first);
        }
        else
        {
            schedule = std::string(first);
            bool complete = true;
            for (int field = 1; field < 5; ++field)
            {
                const std::string_view word = Detail::nextWord(rest);
                if (word.empty())
                {
                    complete = false;
                    break;
                }
                schedule += ' ';
                schedule += word;
            }
            if (!complete)
            {
                continue;
            }
        }
        std::string user;
        if (withUser)
        {
            user = std::string(Detail::nextWord(rest));
        }
        const std::string_view command = Detail::trim(rest);
        if (command.empty() || (withUser && user.empty()))
        {
            continue;
        }
        entries.push_back(
            Entry{.schedule = std::move(schedule), .user = std::move(user), .command = std::string(command), .line = lineNumber});
    }
    return entries;
}

/// How the Startup tab words @p schedule: "at startup" for @reboot, else as written ("@daily", "*/5 * * * *").
[[nodiscard]] inline std::string describeSchedule(std::string_view schedule)
{
    return schedule == "@reboot" ? std::string("at startup") : std::string(schedule);
}

} // namespace Platform::CronTab
