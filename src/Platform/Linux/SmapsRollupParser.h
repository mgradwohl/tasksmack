#pragma once

// /proc/[pid]/smaps_rollup as a process's PSS and USS (#1885): a pure parser, free of any system call,
// so it is unit tested on every platform (tests/Platform/test_SmapsRollupParser.cpp).
//
// The file is one header line (the rolled-up address range, "[rollup]") and then "Key:  value kB"
// lines summed over every mapping, among them
//   Pss:             6789 kB
//   Private_Clean:    100 kB
//   Private_Dirty:    200 kB
//   SwapPss:           50 kB
// PSS is Pss; USS is Private_Clean + Private_Dirty, the pages mapped by this process alone. SwapPss
// is absent on kernels before 4.3 and then reads 0.

#include "Platform/IProcessProbe.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::SmapsRollup
{

namespace Detail
{

/// The kB value of a "Key:  value kB" line's remainder @p rest (after the colon), in bytes; nullopt when
/// it isn't one or would overflow.
[[nodiscard]] inline std::optional<std::uint64_t> kilobytes(std::string_view rest)
{
    const std::size_t start = rest.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }
    rest.remove_prefix(start);
    std::uint64_t kb = 0;
    const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), kb);
    if (ec != std::errc{} || ptr == rest.data())
    {
        return std::nullopt;
    }
    rest.remove_prefix(static_cast<std::size_t>(ptr - rest.data()));
    if (rest.find_first_not_of(" \t") == std::string_view::npos || rest.substr(rest.find_first_not_of(" \t"), 2) != "kB")
    {
        return std::nullopt;
    }
    if (kb > std::numeric_limits<std::uint64_t>::max() / 1024U)
    {
        return std::nullopt;
    }
    return kb * 1024U;
}

} // namespace Detail

/// The PSS, USS and swap PSS in @p text, the contents of /proc/[pid]/smaps_rollup; nullopt when Pss or
/// either Private line is missing or unreadable (not a rollup, or a truncated read).
[[nodiscard]] inline std::optional<MemoryRollup> parse(std::string_view text)
{
    std::optional<std::uint64_t> pss;
    std::optional<std::uint64_t> privateClean;
    std::optional<std::uint64_t> privateDirty;
    std::uint64_t swapPss = 0;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        const std::string_view rest = line.substr(colon + 1);
        if (key == "Pss")
        {
            pss = Detail::kilobytes(rest);
        }
        else if (key == "Private_Clean")
        {
            privateClean = Detail::kilobytes(rest);
        }
        else if (key == "Private_Dirty")
        {
            privateDirty = Detail::kilobytes(rest);
        }
        else if (key == "SwapPss")
        {
            swapPss = Detail::kilobytes(rest).value_or(0);
        }
    }
    if (!pss.has_value() || !privateClean.has_value() || !privateDirty.has_value())
    {
        return std::nullopt;
    }
    if (*privateClean > std::numeric_limits<std::uint64_t>::max() - *privateDirty)
    {
        return std::nullopt;
    }
    return MemoryRollup{.pssBytes = *pss, .ussBytes = *privateClean + *privateDirty, .swapPssBytes = swapPss};
}

} // namespace Platform::SmapsRollup
