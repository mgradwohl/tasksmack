#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::ProcPrivileges
{

/// capability.h bit numbers of the two capabilities that open other users' /proc entries.
inline constexpr unsigned CAP_DAC_READ_SEARCH_BIT = 2; // list another user's /proc/[pid]/fd, open its io
inline constexpr unsigned CAP_SYS_PTRACE_BIT = 19;     // ptrace read access: /proc/[pid]/io, /proc/[pid]/fd/* links

/// Both bits together: what docs/guide/faq.md tells users to grant with setcap.
inline constexpr std::uint64_t FULL_PROC_ACCESS_MASK =
    (std::uint64_t{1} << CAP_DAC_READ_SEARCH_BIT) | (std::uint64_t{1} << CAP_SYS_PTRACE_BIT);

/// The effective capability set from /proc/[pid]/status ("CapEff:\t000001ffffffffff"), or nullopt
/// when the line is missing or its value isn't hex. Deliberately free of platform APIs so it is
/// unit-testable on any host.
[[nodiscard]] inline std::optional<std::uint64_t> parseCapEff(std::string_view status) noexcept
{
    constexpr std::string_view KEY = "CapEff:";
    std::size_t lineStart = 0;
    while (lineStart < status.size())
    {
        std::size_t lineEnd = status.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = status.size();
        }
        // Built from pointer and length rather than substr(), which may throw; offsets are in range.
        std::string_view line(status.data() + lineStart, lineEnd - lineStart);
        if (line.starts_with(KEY))
        {
            line.remove_prefix(KEY.size());
            const std::size_t valueStart = line.find_first_not_of(" \t");
            if (valueStart == std::string_view::npos)
            {
                return std::nullopt;
            }
            line.remove_prefix(valueStart);
            const std::size_t valueEnd = line.find_first_of(" \t\r");
            if (valueEnd != std::string_view::npos)
            {
                line.remove_suffix(line.size() - valueEnd);
            }
            std::uint64_t value = 0;
            const char* const first = line.data();
            const char* const last = first + line.size();
            const auto [ptr, ec] = std::from_chars(first, last, value, 16);
            if (ec != std::errc{} || ptr != last)
            {
                return std::nullopt;
            }
            return value;
        }
        lineStart = lineEnd + 1;
    }
    return std::nullopt;
}

/// Whether running with more privileges would restore per-process data for other users' processes
/// (ProcessCapabilities::hasReducedPrivileges). Not reduced as root, or with both CAP_DAC_READ_SEARCH
/// and CAP_SYS_PTRACE effective. Anything less is reduced, including CAP_DAC_READ_SEARCH alone: that
/// restores FD counts but not I/O or network, so the notice (which names all three) still applies.
/// An unknown capability set (status unreadable) is treated as none.
[[nodiscard]] constexpr bool hasReducedPrivileges(bool isRoot, std::optional<std::uint64_t> capEff) noexcept
{
    if (isRoot)
    {
        return false;
    }
    return !capEff.has_value() || (*capEff & FULL_PROC_ACCESS_MASK) != FULL_PROC_ACCESS_MASK;
}

} // namespace Platform::ProcPrivileges
