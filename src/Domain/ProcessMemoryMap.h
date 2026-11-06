#pragma once

// A process's memory map summed up for Process Details (#3089): Platform reads the mappings raw
// (Platform/IProcessMemoryMap.h, smaps on Linux, VirtualQueryEx on Windows); these give the totals over
// all of them, by kind and by mapped file, each kind's label and a mapping's protection text, once per read,
// never per frame. Both platforms' views use them.
//
// A size the platform didn't measure stays absent in a total: a total is absent only when no mapping in it
// had that size, so Windows' rows, which carry none of Linux's smaps sizes, sum to absent ones rather than
// to zeros a view would show as "0 B".

#include "Platform/IProcessMemoryMap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Domain::MemoryMap
{

/// Sums over some mappings.
struct Totals
{
    std::size_t mappings = 0;        ///< How many mappings were summed.
    std::uint64_t sizeBytes = 0;     ///< Their address space, reserved regions left out.
    std::uint64_t reservedBytes = 0; ///< Reserved address space (Windows MEM_RESERVE; 0 on Linux).
    // NOLINTBEGIN(readability-redundant-member-init) - {} lets designated initializers omit them without a warning
    std::optional<std::uint64_t> residentBytes{};
    std::optional<std::uint64_t> pssBytes{};
    std::optional<std::uint64_t> privateBytes{}; ///< Private clean + dirty: what freeing this would give back (USS).
    std::optional<std::uint64_t> sharedBytes{};  ///< Shared clean + dirty.
    std::optional<std::uint64_t> swapBytes{};
    // NOLINTEND(readability-redundant-member-init)

    friend bool operator==(const Totals&, const Totals&) = default;
};

/// One kind's totals.
struct KindTotals
{
    Platform::MemoryMappingKind kind = Platform::MemoryMappingKind::Anonymous;
    Totals totals;

    friend bool operator==(const KindTotals&, const KindTotals&) = default;
};

/// One mapped file's totals: every Image or File mapping of it.
struct FileTotals
{
    std::string path;
    bool deleted = false; ///< A deleted file, not the one now at its path.
    Totals totals;

    friend bool operator==(const FileTotals&, const FileTotals&) = default;
};

/// summarize()'s answer.
struct Summary
{
    Totals all;
    std::vector<KindTotals> byKind; ///< Only the kinds present, largest first.
    std::vector<FileTotals> byFile; ///< Every mapped file, largest first.

    friend bool operator==(const Summary&, const Summary&) = default;
};

namespace Detail
{

/// @p left + @p right, at most UINT64_MAX.
[[nodiscard]] constexpr std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

/// @p total plus @p size: absent only while every size added was.
constexpr void add(std::optional<std::uint64_t>& total, const std::optional<std::uint64_t>& size) noexcept
{
    if (size.has_value())
    {
        total = saturatingAdd(total.value_or(0), *size);
    }
}

/// @p first + @p second when either is present.
[[nodiscard]] constexpr std::optional<std::uint64_t> sum(const std::optional<std::uint64_t>& first,
                                                         const std::optional<std::uint64_t>& second) noexcept
{
    std::optional<std::uint64_t> total;
    add(total, first);
    add(total, second);
    return total;
}

/// What "largest" means when sorting totals: PSS, then resident, then address space, each when known.
[[nodiscard]] constexpr auto weight(const Totals& totals) noexcept
{
    return std::make_tuple(totals.pssBytes.value_or(0), totals.residentBytes.value_or(0), totals.sizeBytes);
}

} // namespace Detail

/// @p mapping added to @p totals.
constexpr void accumulate(Totals& totals, const Platform::MemoryMapping& mapping) noexcept
{
    ++totals.mappings;
    if (mapping.reserved)
    {
        totals.reservedBytes = Detail::saturatingAdd(totals.reservedBytes, mapping.sizeBytes());
    }
    else
    {
        totals.sizeBytes = Detail::saturatingAdd(totals.sizeBytes, mapping.sizeBytes());
    }
    Detail::add(totals.residentBytes, mapping.residentBytes);
    Detail::add(totals.pssBytes, mapping.pssBytes);
    Detail::add(totals.privateBytes, Detail::sum(mapping.privateCleanBytes, mapping.privateDirtyBytes));
    Detail::add(totals.sharedBytes, Detail::sum(mapping.sharedCleanBytes, mapping.sharedDirtyBytes));
    Detail::add(totals.swapBytes, mapping.swapBytes);
}

/// The totals of @p mappings: over all of them, by kind and by file, each list largest first (PSS, then
/// resident, then address space; ties by kind order or by path).
[[nodiscard]] inline Summary summarize(std::span<const Platform::MemoryMapping> mappings)
{
    Summary summary;
    std::map<Platform::MemoryMappingKind, Totals> kinds;
    std::map<std::pair<std::string_view, bool>, Totals> files;
    for (const Platform::MemoryMapping& mapping : mappings)
    {
        accumulate(summary.all, mapping);
        accumulate(kinds[mapping.kind], mapping);
        if (mapping.kind == Platform::MemoryMappingKind::Image || mapping.kind == Platform::MemoryMappingKind::File)
        {
            accumulate(files[{mapping.path, mapping.deleted}], mapping);
        }
    }
    for (const auto& [kind, totals] : kinds)
    {
        summary.byKind.push_back({.kind = kind, .totals = totals});
    }
    for (const auto& [file, totals] : files)
    {
        summary.byFile.push_back({.path = std::string(file.first), .deleted = file.second, .totals = totals});
    }
    // Stable, over lists already in kind and path order: equal weights keep that order.
    std::ranges::stable_sort(summary.byKind,
                             [](const KindTotals& a, const KindTotals& b) { return Detail::weight(a.totals) > Detail::weight(b.totals); });
    std::ranges::stable_sort(summary.byFile,
                             [](const FileTotals& a, const FileTotals& b) { return Detail::weight(a.totals) > Detail::weight(b.totals); });
    return summary;
}

/// A kind's name for a column or a heading.
[[nodiscard]] constexpr std::string_view kindLabel(Platform::MemoryMappingKind kind) noexcept
{
    switch (kind)
    {
    case Platform::MemoryMappingKind::Image:
        return "Image";
    case Platform::MemoryMappingKind::File:
        return "Mapped file";
    case Platform::MemoryMappingKind::Anonymous:
        return "Private";
    case Platform::MemoryMappingKind::Heap:
        return "Heap";
    case Platform::MemoryMappingKind::Stack:
        return "Stack";
    case Platform::MemoryMappingKind::System:
        return "System";
    case Platform::MemoryMappingKind::Other:
        return "Other";
    }
    return "Other";
}

/// @p mapping's protection as /proc/[pid]/maps writes it ("r-xp", "rw-s"), with a "g" after it for a guard
/// page (Windows).
[[nodiscard]] inline std::string protectionText(const Platform::MemoryMapping& mapping)
{
    std::string text;
    text += mapping.readable ? 'r' : '-';
    text += mapping.writable ? 'w' : '-';
    text += mapping.executable ? 'x' : '-';
    text += mapping.shared ? 's' : 'p';
    if (mapping.guard)
    {
        text += 'g';
    }
    return text;
}

} // namespace Domain::MemoryMap
