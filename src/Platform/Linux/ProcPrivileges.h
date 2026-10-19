#pragma once

#include "Platform/ProcessTypes.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/types.h>

namespace Platform::ProcPrivileges
{

/// This process's supplementary groups, through @p getGroups (getgroups(2), or a stand-in with its
/// signature in tests). Sized by asking first: a fixed buffer fails with EINVAL for a user in more groups
/// than it holds (#2400). Empty if they can't be read.
template<typename GetGroups> [[nodiscard]] std::vector<gid_t> supplementaryGroups(GetGroups getGroups)
{
    const int wanted = getGroups(0, nullptr);
    std::vector<gid_t> groups(static_cast<std::size_t>(wanted > 0 ? wanted : 0));
    const int count = groups.empty() ? 0 : getGroups(static_cast<int>(groups.size()), groups.data());
    groups.resize(static_cast<std::size_t>(count > 0 ? count : 0));
    return groups;
}

/// capability.h bit numbers of the capabilities that open other users' /proc entries.
inline constexpr unsigned CAP_DAC_OVERRIDE_BIT = 1;    // a superset of CAP_DAC_READ_SEARCH for these reads
inline constexpr unsigned CAP_DAC_READ_SEARCH_BIT = 2; // list another user's /proc/[pid]/fd, open its io
inline constexpr unsigned CAP_KILL_BIT = 5;            // signal another user's process
inline constexpr unsigned CAP_SYS_PTRACE_BIT = 19;     // ptrace read access: /proc/[pid]/io, /proc/[pid]/fd/* links
inline constexpr unsigned CAP_SYS_ADMIN_BIT = 21;      // also lets ioprio_set() use the Realtime class
inline constexpr unsigned CAP_SYS_NICE_BIT = 23;       // ioprio_set(IOPRIO_CLASS_RT), raising priority

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
/// (ProcessCapabilities::hasReducedPrivileges). When the effective set is known it decides, for root
/// too: a process can keep EUID 0 with its capabilities dropped (a container, a hardened service) and
/// then can't read other users' entries either. Not reduced with CAP_SYS_PTRACE plus
/// CAP_DAC_READ_SEARCH (or CAP_DAC_OVERRIDE, which covers it); anything less is reduced, including
/// CAP_DAC_READ_SEARCH alone: that restores FD counts but not I/O or network, so the notice (which
/// names all three) still applies. With the set unknown (status unreadable), only root counts as not
/// reduced.
[[nodiscard]] constexpr bool hasReducedPrivileges(bool isRoot, std::optional<std::uint64_t> capEff) noexcept
{
    if (!capEff.has_value())
    {
        return !isRoot;
    }
    constexpr std::uint64_t DAC_MASK = (std::uint64_t{1} << CAP_DAC_READ_SEARCH_BIT) | (std::uint64_t{1} << CAP_DAC_OVERRIDE_BIT);
    const bool dacAccess = (*capEff & DAC_MASK) != 0;
    const bool ptraceAccess = (*capEff & (std::uint64_t{1} << CAP_SYS_PTRACE_BIT)) != 0;
    return !(dacAccess && ptraceAccess);
}

/// Whether @p capEff (parseCapEff() of TaskSmack's own status) lets ioprio_set() use the Realtime
/// class: CAP_SYS_NICE or CAP_SYS_ADMIN (ioprio_set(2)). Unknown is no: the slider then leaves Realtime
/// out rather than offer a class every attempt would refuse (#1540).
[[nodiscard]] constexpr bool canSetRealtimeIoPriority(std::optional<std::uint64_t> capEff) noexcept
{
    return capEff.has_value() && (((*capEff >> CAP_SYS_NICE_BIT) & 1U) != 0 || ((*capEff >> CAP_SYS_ADMIN_BIT) & 1U) != 0);
}

/// How /proc is mounted, as far as other users' processes go (#2303): its hidepid= level and gid=.
struct ProcMount
{
    /// hidepid: 0 (off), 1 (noaccess: listed, but their directories refused, so no row can be read),
    /// 2 (invisible: not listed) or 4 (ptraceable: only processes the caller may ptrace are listed).
    int hidepid = 0;
    /// gid=: members of this group see every process anyway. nullopt when not set.
    std::optional<std::uint32_t> gid;
};

/// The /proc mount's hidepid= and gid= from /proc/self/mountinfo, or nullopt when no line mounts /proc
/// (the last one wins: a later mount hides an earlier one). Each line is "id parent maj:min root
/// mountpoint options [optional fields] - fstype source superoptions"; hidepid and gid are among the
/// superoptions. hidepid is also written as a word: off, noaccess, invisible, ptraceable.
[[nodiscard]] inline std::optional<ProcMount> parseProcMount(std::string_view mountinfo)
{
    const auto nextField = [](std::string_view& text) -> std::string_view
    {
        const std::size_t start = text.find_first_not_of(' ');
        if (start == std::string_view::npos)
        {
            text = {};
            return {};
        }
        text.remove_prefix(start);
        const std::size_t end = text.find(' ');
        const std::string_view field = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end);
        return field;
    };
    const auto parseUnsigned = [](std::string_view text) -> std::optional<std::uint32_t>
    {
        std::uint32_t value = 0;
        const char* const first = text.data();
        const char* const last = first + text.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        return (ec == std::errc{} && ptr == last && !text.empty()) ? std::optional<std::uint32_t>(value) : std::nullopt;
    };
    std::optional<ProcMount> found;
    while (!mountinfo.empty())
    {
        const std::size_t newline = mountinfo.find('\n');
        std::string_view line = mountinfo.substr(0, newline);
        mountinfo.remove_prefix(newline == std::string_view::npos ? mountinfo.size() : newline + 1);
        for (int skip = 0; skip < 4; ++skip)
        {
            static_cast<void>(nextField(line));
        }
        if (nextField(line) != "/proc")
        {
            continue;
        }
        const std::size_t separator = line.find(" - ");
        if (separator == std::string_view::npos)
        {
            continue;
        }
        line.remove_prefix(separator + 3);
        if (nextField(line) != "proc")
        {
            continue;
        }
        static_cast<void>(nextField(line)); // the source
        std::string_view options = nextField(line);
        ProcMount mount;
        while (!options.empty())
        {
            const std::size_t comma = options.find(',');
            const std::string_view option = options.substr(0, comma);
            options.remove_prefix(comma == std::string_view::npos ? options.size() : comma + 1);
            if (option.starts_with("hidepid="))
            {
                const std::string_view value = option.substr(8);
                if (value == "noaccess")
                {
                    mount.hidepid = 1;
                }
                else if (value == "invisible")
                {
                    mount.hidepid = 2;
                }
                else if (value == "ptraceable")
                {
                    mount.hidepid = 4;
                }
                else if (value == "off")
                {
                    mount.hidepid = 0;
                }
                else if (const auto level = parseUnsigned(value); level.has_value())
                {
                    mount.hidepid = static_cast<int>(*level);
                }
            }
            else if (option.starts_with("gid="))
            {
                mount.gid = parseUnsigned(option.substr(4));
            }
        }
        found = mount;
    }
    return found;
}

/// The permissions TaskSmack is missing, as MissingPrivilege bits (#2303), from its root-ness, its
/// effective capabilities (parseCapEff(); nullopt when unreadable), how /proc is mounted, and whether it
/// is in the mount's gid= group.
/// - READ_OTHERS: hasReducedPrivileges() (CAP_SYS_PTRACE + CAP_DAC_READ_SEARCH or CAP_DAC_OVERRIDE).
/// - SIGNAL_OTHERS: no CAP_KILL. RAISE_PRIORITY: no CAP_SYS_NICE.
/// - PROCESSES_HIDDEN: hidepid set at all (noaccess, invisible or ptraceable), outside its group and
///   without CAP_SYS_PTRACE. noaccess lists the PIDs but refuses opening their directories, so
///   TaskSmack can't show those processes either (#2399).
/// With the capabilities unknown, root is taken to have them all and anyone else none of them.
[[nodiscard]] constexpr std::uint8_t
missingPrivileges(bool isRoot, std::optional<std::uint64_t> capEff, std::optional<ProcMount> procMount, bool inProcMountGroup) noexcept
{
    namespace Missing = MissingPrivilege;
    const auto has = [&](unsigned bit)
    {
        return capEff.has_value() ? ((*capEff >> bit) & 1U) != 0 : isRoot;
    };
    std::uint8_t missing = 0;
    if (hasReducedPrivileges(isRoot, capEff))
    {
        missing |= Missing::READ_OTHERS;
    }
    if (!has(CAP_KILL_BIT))
    {
        missing |= Missing::SIGNAL_OTHERS;
    }
    if (!has(CAP_SYS_NICE_BIT))
    {
        missing |= Missing::RAISE_PRIORITY;
    }
    if (procMount.has_value() && procMount->hidepid != 0 && !inProcMountGroup && !has(CAP_SYS_PTRACE_BIT))
    {
        missing |= Missing::PROCESSES_HIDDEN;
    }
    return missing;
}

} // namespace Platform::ProcPrivileges
