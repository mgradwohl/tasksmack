#pragma once

// Parsers for the security fields of /proc/[pid]/status, /proc/[pid]/attr/current and
// /proc/[pid]/cgroup (#1526). Pure text in, values out: free of platform APIs so they are unit-tested
// (tests/Platform/test_ProcStatusSecurityParser.cpp) and fuzzed on any host. Names for the IDs are
// looked up by the reader (LinuxProcessSecurityReader), not here.

#include "Platform/IProcessSecurity.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::ProcStatusSecurity
{

/// The value of the "@p key:" line of @p status, with the surrounding blanks trimmed, or nullopt when
/// there is no such line. @p key is the field name without its colon ("Uid", "CapEff").
[[nodiscard]] inline std::optional<std::string_view> fieldValue(std::string_view status, std::string_view key) noexcept
{
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
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == ':')
        {
            line.remove_prefix(key.size() + 1);
            const std::size_t first = line.find_first_not_of(" \t");
            if (first == std::string_view::npos)
            {
                return std::string_view{};
            }
            line.remove_prefix(first);
            const std::size_t last = line.find_last_not_of(" \t\r");
            line.remove_suffix(line.size() - (last + 1));
            return line;
        }
        lineStart = lineEnd + 1;
    }
    return std::nullopt;
}

/// The whitespace-separated unsigned decimal numbers of @p text, or nullopt if any token isn't one.
[[nodiscard]] inline std::optional<std::vector<std::uint32_t>> parseIdList(std::string_view text)
{
    std::vector<std::uint32_t> ids;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        pos = text.find_first_not_of(" \t", pos);
        if (pos == std::string_view::npos)
        {
            break;
        }
        std::size_t end = text.find_first_of(" \t", pos);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        std::uint32_t value = 0;
        const char* const first = text.data() + pos;
        const char* const last = text.data() + end;
        const auto [ptr, ec] = std::from_chars(first, last, value, 10);
        if (ec != std::errc{} || ptr != last)
        {
            return std::nullopt;
        }
        ids.push_back(value);
        pos = end;
    }
    return ids;
}

/// The four IDs of a "Uid:" or "Gid:" value (real, effective, saved, filesystem), names left empty;
/// nullopt unless it holds exactly four numbers.
[[nodiscard]] inline std::optional<SecurityIdSet> parseIdSet(std::string_view value)
{
    const auto ids = parseIdList(value);
    if (!ids.has_value() || ids->size() != 4)
    {
        return std::nullopt;
    }
    return SecurityIdSet{
        .real = {.id = (*ids)[0], .name = {}},
        .effective = {.id = (*ids)[1], .name = {}},
        .saved = {.id = (*ids)[2], .name = {}},
        .filesystem = {.id = (*ids)[3], .name = {}},
    };
}

/// A capability mask value ("000001ffffffffff"), or nullopt when it isn't all hex.
[[nodiscard]] inline std::optional<std::uint64_t> parseCapabilityMask(std::string_view value) noexcept
{
    if (value.empty())
    {
        return std::nullopt;
    }
    std::uint64_t mask = 0;
    const char* const first = value.data();
    const char* const last = first + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, mask, 16);
    if (ec != std::errc{} || ptr != last)
    {
        return std::nullopt;
    }
    return mask;
}

/// The security fields of /proc/[pid]/status. A field that is missing or malformed is left empty.
[[nodiscard]] inline ProcessSecurity parseStatus(std::string_view status)
{
    ProcessSecurity security;
    if (const auto uid = fieldValue(status, "Uid"))
    {
        security.users = parseIdSet(*uid);
    }
    if (const auto gid = fieldValue(status, "Gid"))
    {
        security.groups = parseIdSet(*gid);
    }
    if (const auto groups = fieldValue(status, "Groups"))
    {
        if (const auto ids = parseIdList(*groups))
        {
            security.supplementaryGroups.reserve(ids->size());
            for (const std::uint32_t id : *ids)
            {
                security.supplementaryGroups.push_back({.id = id, .name = {}});
            }
        }
    }
    const auto mask = [&status](std::string_view key) -> std::optional<std::uint64_t>
    {
        const auto value = fieldValue(status, key);
        return value.has_value() ? parseCapabilityMask(*value) : std::nullopt;
    };
    security.capabilities = CapabilitySets{
        .effective = mask("CapEff"),
        .permitted = mask("CapPrm"),
        .inheritable = mask("CapInh"),
        .bounding = mask("CapBnd"),
        .ambient = mask("CapAmb"),
    };
    if (const auto noNewPrivs = fieldValue(status, "NoNewPrivs"))
    {
        if (*noNewPrivs == "0" || *noNewPrivs == "1")
        {
            security.noNewPrivileges = (*noNewPrivs == "1");
        }
    }
    if (const auto seccomp = fieldValue(status, "Seccomp"))
    {
        if (*seccomp == "0" || *seccomp == "1" || *seccomp == "2")
        {
            security.seccomp = static_cast<SeccompMode>((*seccomp)[0] - '0');
        }
    }
    return security;
}

/// /proc/[pid]/attr/current as a label: up to its first NUL or newline, trailing blanks trimmed. Empty
/// when there is no LSM label.
[[nodiscard]] inline std::string parseSecurityLabel(std::string_view current)
{
    // substr() from 0 never throws; npos keeps the whole of it.
    const std::string_view label = current.substr(0, current.find_first_of(std::string_view("\0\n", 2)));
    const std::size_t last = label.find_last_not_of(" \t\r");
    if (last == std::string_view::npos)
    {
        return {};
    }
    return std::string{label.substr(0, last + 1)};
}

/// The cgroup /proc/[pid]/cgroup places a process in: the cgroup v2 path (the "0::" line) when there
/// is one, else every v1 line ("hierarchy:controllers:path") joined with "; ". Empty when unreadable.
[[nodiscard]] inline std::string parseControlGroup(std::string_view cgroup)
{
    std::string v1;
    std::size_t lineStart = 0;
    while (lineStart < cgroup.size())
    {
        std::size_t lineEnd = cgroup.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = cgroup.size();
        }
        const std::string_view line(cgroup.data() + lineStart, lineEnd - lineStart);
        if (line.starts_with("0::"))
        {
            return std::string(line.substr(3));
        }
        if (!line.empty())
        {
            if (!v1.empty())
            {
                v1 += "; ";
            }
            v1 += line;
        }
        lineStart = lineEnd + 1;
    }
    return v1;
}

} // namespace Platform::ProcStatusSecurity
