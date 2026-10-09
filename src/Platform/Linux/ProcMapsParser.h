#pragma once

// /proc/[pid]/maps as loaded modules (#802): a pure parser, free of any system call, so it is unit
// tested and fuzzed on every platform (tests/Platform/test_ProcMapsParser.cpp, tests/fuzz/fuzz_proc_maps.cpp).
//
// Each maps line is
//   start-end perms offset dev inode [pathname]
//   7f3a1c000000-7f3a1c028000 r--p 00000000 08:01 1835 /usr/lib/x86_64-linux-gnu/libc.so.6
// with the pathname padded out to a column, possibly containing spaces, and " (deleted)" appended
// when the file was unlinked or replaced after it was mapped.
//
// A module is a file with at least one executable mapping: the main executable and every shared
// object. A file mapped only for data (a locale archive, a font) is not, nor are anonymous mappings
// and the kernel's [heap], [stack], [vdso] and the like. A module's mappings are grouped by pathname:
// its base is the lowest start, its size the sum of the spans. A deleted file and the file now at
// its path are different files, so they stay separate rows.

#include "Platform/IProcessModules.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Platform::ProcMaps
{

/// One maps line's fields that matter here. `pathname` is a view into the parsed text, with any
/// " (deleted)" left on.
struct MapsLine
{
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool executable = false;
    std::uint64_t inode = 0;
    std::string_view pathname;
};

inline constexpr std::string_view DELETED_SUFFIX = " (deleted)";

namespace Detail
{

/// Parses the number at the front of @p rest in @p base and drops it and the one @p separator after it.
template<typename T> [[nodiscard]] bool takeNumber(std::string_view& rest, int base, char separator, T& out) noexcept
{
    const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), out, base);
    const auto used = static_cast<std::size_t>(ptr - rest.data());
    if (ec != std::errc{} || used == 0 || used >= rest.size() || rest[used] != separator)
    {
        return false;
    }
    rest.remove_prefix(used + 1);
    return true;
}

} // namespace Detail

/// Parses one maps line (without its newline); nullopt for a line that is cut short or malformed.
[[nodiscard]] inline std::optional<MapsLine> parseMapsLine(std::string_view line) noexcept
{
    MapsLine out;
    std::uint64_t offset = 0;
    if (!Detail::takeNumber(line, 16, '-', out.start) || !Detail::takeNumber(line, 16, ' ', out.end) || out.end < out.start)
    {
        return std::nullopt;
    }
    // perms: exactly four characters, "r-xp"
    if (line.size() < 5 || line[4] != ' ')
    {
        return std::nullopt;
    }
    out.executable = line[2] == 'x';
    line.remove_prefix(5);
    if (!Detail::takeNumber(line, 16, ' ', offset))
    {
        return std::nullopt;
    }
    // dev: "08:01" (major:minor, hex)
    const std::size_t devEnd = line.find(' ');
    if (devEnd == std::string_view::npos || line.find(':') > devEnd)
    {
        return std::nullopt;
    }
    line.remove_prefix(devEnd + 1);
    // inode, then the padding before the pathname; an anonymous mapping ends at the inode
    const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), out.inode, 10);
    const auto used = static_cast<std::size_t>(ptr - line.data());
    if (ec != std::errc{} || used == 0 || (used < line.size() && line[used] != ' '))
    {
        return std::nullopt;
    }
    line.remove_prefix(used);
    line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
    out.pathname = line;
    return out;
}

/// The modules in a whole maps file, in address order of their first mapping. Lines that cannot be
/// parsed (a truncated last line, say) are skipped.
[[nodiscard]] inline std::vector<ProcessModule> parseProcMaps(std::string_view text)
{
    struct Group
    {
        std::string_view pathname; // with " (deleted)" when it has it
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        bool executable = false;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string_view, std::size_t> indexOf;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

        const std::optional<MapsLine> parsed = parseMapsLine(line);
        // A file: a path and a real inode. "[heap]", "[anon:...]", "anon_inode:..." and anonymous
        // mappings are not.
        if (!parsed || parsed->inode == 0 || !parsed->pathname.starts_with('/'))
        {
            continue;
        }
        const auto [it, inserted] = indexOf.try_emplace(parsed->pathname, groups.size());
        if (inserted)
        {
            groups.push_back({.pathname = parsed->pathname, .base = parsed->start, .size = 0, .executable = false});
        }
        Group& group = groups[it->second];
        group.base = std::min(group.base, parsed->start);
        group.size += parsed->end - parsed->start;
        group.executable = group.executable || parsed->executable;
    }

    std::vector<ProcessModule> modules;
    for (const Group& group : groups)
    {
        if (!group.executable)
        {
            continue; // mapped for data only
        }
        const bool deleted = group.pathname.ends_with(DELETED_SUFFIX);
        const std::string_view path = deleted ? group.pathname.substr(0, group.pathname.size() - DELETED_SUFFIX.size()) : group.pathname;
        modules.push_back(
            {.path = std::string(path), .baseAddress = group.base, .sizeBytes = group.size, .version = std::nullopt, .deleted = deleted});
    }
    return modules;
}

} // namespace Platform::ProcMaps
