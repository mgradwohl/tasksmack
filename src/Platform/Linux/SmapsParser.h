#pragma once

// /proc/[pid]/smaps as a memory map (#3089): a pure parser, free of any system call, so it is unit tested
// on every platform (tests/Platform/test_SmapsParser.cpp).
//
// The file is one entry per mapping: the mapping's /proc/[pid]/maps line (ProcMapsParser.h), then
// "Key:  value kB" lines measured over that mapping alone, among them
//   7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835   /usr/lib/libc.so.6
//   Size:               1620 kB
//   Rss:                1200 kB
//   Pss:                  40 kB
//   Shared_Clean:       1200 kB
//   Shared_Dirty:          0 kB
//   Private_Clean:         0 kB
//   Private_Dirty:         0 kB
//   Swap:                  0 kB
//   SwapPss:               0 kB
//   VmFlags: rd ex mr mw me sd
// A line without " kB" (VmFlags, THPeligible, ProtectionKey) is not a size and is skipped. A size the entry
// lacks (SwapPss before Linux 4.3), or one that doesn't read or would overflow in bytes, is left absent
// rather than read as 0. A header line that doesn't parse is skipped with the size lines after it, so a
// mapping is never given another's sizes.
//
// Each mapping's kind follows its pathname: none is Anonymous; [heap], [stack] and the kernel's [vdso],
// [vvar], [vsyscall] and the like are Heap, Stack and System; a path is a file, an Image when that file has
// an executable mapping anywhere in the map (as ProcMapsParser groups modules) and File otherwise; any
// other name ([anon:name], anon_inode:...) is Other. " (deleted)" is taken off a file's path and kept as
// `deleted`, and a deleted file and the file now at its path are different files.

#include "Platform/IProcessMemoryMap.h"
#include "ProcMapsParser.h"
#include "SmapsRollupParser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform::Smaps
{

namespace Detail
{

/// Whether @p line is a "Key: value" line rather than a mapping's header: its text before the first colon is
/// a nonempty run of letters, digits and underscores. A header's range has a '-' before its first colon (the
/// one in its device), so it never is one.
[[nodiscard]] constexpr bool isFieldLine(std::string_view line) noexcept
{
    const std::size_t colon = line.find(':');
    if (colon == 0 || colon == std::string_view::npos)
    {
        return false;
    }
    // The key: the line up to the colon (substr() can throw, and this is noexcept).
    std::string_view key = line;
    key.remove_suffix(line.size() - colon);
    return std::ranges::all_of(
        key, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

/// The kernel's names for the pages it maps into every process.
inline constexpr std::array<std::string_view, 5> SYSTEM_NAMES{"[vdso]", "[vvar]", "[vvar_vclock]", "[vsyscall]", "[uprobes]"};

/// The size field @p key names in @p mapping, or null when it is one this map doesn't keep.
[[nodiscard]] inline std::optional<std::uint64_t>* sizeField(MemoryMapping& mapping, std::string_view key) noexcept
{
    if (key == "Rss")
    {
        return &mapping.residentBytes;
    }
    if (key == "Pss")
    {
        return &mapping.pssBytes;
    }
    if (key == "Shared_Clean")
    {
        return &mapping.sharedCleanBytes;
    }
    if (key == "Shared_Dirty")
    {
        return &mapping.sharedDirtyBytes;
    }
    if (key == "Private_Clean")
    {
        return &mapping.privateCleanBytes;
    }
    if (key == "Private_Dirty")
    {
        return &mapping.privateDirtyBytes;
    }
    if (key == "Swap")
    {
        return &mapping.swapBytes;
    }
    if (key == "SwapPss")
    {
        return &mapping.swapPssBytes;
    }
    return nullptr;
}

} // namespace Detail

/// The kind a maps @p pathname names, as the header says: a path is File here (parse() makes the files with
/// an executable mapping Image).
[[nodiscard]] constexpr MemoryMappingKind kindOf(std::string_view pathname) noexcept
{
    if (pathname.empty())
    {
        return MemoryMappingKind::Anonymous;
    }
    if (pathname.starts_with('/'))
    {
        return MemoryMappingKind::File;
    }
    if (pathname == "[heap]")
    {
        return MemoryMappingKind::Heap;
    }
    if (pathname == "[stack]" || pathname.starts_with("[stack:")) // [stack:tid] before Linux 4.5
    {
        return MemoryMappingKind::Stack;
    }
    for (const std::string_view name : Detail::SYSTEM_NAMES)
    {
        if (pathname == name)
        {
            return MemoryMappingKind::System;
        }
    }
    return MemoryMappingKind::Other;
}

/// The mappings in a whole smaps file, in its (address) order. Lines that cannot be parsed (a truncated last
/// line, say) are skipped.
[[nodiscard]] inline std::vector<MemoryMapping> parse(std::string_view text)
{
    std::vector<MemoryMapping> mappings;
    std::vector<std::string_view> pathnames;     // each mapping's pathname, " (deleted)" left on: a file's identity
    std::unordered_set<std::string_view> images; // the files with an executable mapping
    bool inMapping = false;                      // the size lines that follow belong to mappings.back()
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

        if (Detail::isFieldLine(line))
        {
            if (!inMapping)
            {
                continue;
            }
            const std::size_t colon = line.find(':');
            if (std::optional<std::uint64_t>* field = Detail::sizeField(mappings.back(), line.substr(0, colon)); field != nullptr)
            {
                *field = SmapsRollup::Detail::kilobytes(line.substr(colon + 1));
            }
            continue;
        }
        const std::optional<ProcMaps::MapsLine> header = ProcMaps::parseMapsLine(line);
        inMapping = header.has_value();
        if (!header.has_value())
        {
            continue;
        }
        MemoryMapping mapping;
        mapping.start = header->start;
        mapping.end = header->end;
        mapping.readable = header->readable;
        mapping.writable = header->writable;
        mapping.executable = header->executable;
        mapping.shared = header->shared;
        mapping.fileOffset = header->offset;
        mapping.kind = kindOf(header->pathname);
        std::string_view path = header->pathname;
        if (mapping.kind == MemoryMappingKind::File)
        {
            mapping.deleted = path.ends_with(ProcMaps::DELETED_SUFFIX);
            if (mapping.deleted)
            {
                path.remove_suffix(ProcMaps::DELETED_SUFFIX.size());
            }
            if (header->executable)
            {
                images.insert(header->pathname);
            }
        }
        mapping.path = std::string(path);
        mappings.push_back(std::move(mapping));
        pathnames.push_back(header->pathname);
    }
    for (std::size_t i = 0; i < mappings.size(); ++i)
    {
        if (mappings[i].kind == MemoryMappingKind::File && images.contains(pathnames[i]))
        {
            mappings[i].kind = MemoryMappingKind::Image;
        }
    }
    return mappings;
}

} // namespace Platform::Smaps
