#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::ProcStat
{

/// Position of starttime among the fields that follow the ")" closing comm in /proc/[pid]/stat.
///
/// proc(5) numbers starttime as field 22, and the fields after comm begin at field 3 (state), so it
/// is the twentieth field after the ")" -- index 19 counting from zero.
inline constexpr std::size_t START_TIME_INDEX_AFTER_COMM = 19;

/// The starttime field of a /proc/[pid]/stat line: clock ticks after boot at which the process
/// started, the value LinuxProcessProbe reports as ProcessCounters::startTimeTicks.
///
/// Fields are counted from the *last* ")", because comm is the executable name as the process set
/// it and may itself contain spaces and parentheses (#973 checks this value before acting on a
/// process, so a crafted name must not be able to shift the field it is read from).
///
/// Deliberately free of platform APIs so it is unit-testable on any host.
///
/// @return The start time, or nullopt if the line is malformed.
[[nodiscard]] inline std::optional<std::uint64_t> parseStartTime(std::string_view statLine) noexcept
{
    const std::size_t commEnd = statLine.rfind(')');
    if (commEnd == std::string_view::npos)
    {
        return std::nullopt;
    }

    // Built from pointer and length rather than substr(), which may throw; every offset here is
    // already known to be in range.
    std::string_view rest(statLine.data() + commEnd + 1, statLine.size() - commEnd - 1);
    for (std::size_t index = 0;; ++index)
    {
        const std::size_t fieldStart = rest.find_first_not_of(" \n");
        if (fieldStart == std::string_view::npos)
        {
            return std::nullopt;
        }
        rest.remove_prefix(fieldStart);
        const std::size_t fieldEnd = std::min(rest.find_first_of(" \n"), rest.size());

        if (index == START_TIME_INDEX_AFTER_COMM)
        {
            const char* const fieldBegin = rest.data();
            const char* const fieldLast = rest.data() + fieldEnd;
            std::uint64_t value = 0;
            const auto [ptr, ec] = std::from_chars(fieldBegin, fieldLast, value);
            if (ec != std::errc{} || ptr != fieldLast)
            {
                return std::nullopt;
            }
            return value;
        }
        rest.remove_prefix(fieldEnd);
    }
}

} // namespace Platform::ProcStat
