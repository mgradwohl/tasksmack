#pragma once

// The Linux Commit & paging facts (#1516), read under an injected root ("/" in the app, a fixture tree in
// tests): Committed_AS, CommitLimit and HugePages_* from /proc/meminfo, vm.overcommit_memory, the swap
// devices in /proc/swaps, each zram device's /sys/block/zramN/mm_stat, zswap's enabled parameter and the
// transparent huge page mode. All unprivileged; debugfs (zswap's statistics) is never read and nothing is
// spawned. A file that can't be read leaves its facts unknown. The parsers are pure and standard library
// only, so the fixture tests and the fuzz target (tests/fuzz/fuzz_commit_paging.cpp) run everywhere.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxOsInfo.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxCommitPaging
{

/// @p text's whitespace-separated fields.
[[nodiscard]] inline std::vector<std::string_view> splitFields(std::string_view text)
{
    std::vector<std::string_view> fields;
    std::size_t start = text.find_first_not_of(" \t\r\n");
    while (start != std::string_view::npos)
    {
        const std::size_t end = text.find_first_of(" \t\r\n", start);
        fields.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        start = end == std::string_view::npos ? end : text.find_first_not_of(" \t\r\n", end);
    }
    return fields;
}

/// A whole field as an unsigned number; nullopt when it isn't one.
[[nodiscard]] inline std::optional<std::uint64_t> parseUnsigned(std::string_view field)
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{} || end != field.data() + field.size() || field.empty())
    {
        return std::nullopt;
    }
    return value;
}

/// The number on /proc/meminfo's "@p key:" line ("Committed_AS:  1234 kB" -> 1234, in the file's unit);
/// nullopt when the key is missing or its value isn't a number.
[[nodiscard]] inline std::optional<std::uint64_t> meminfoValue(std::string_view meminfo, std::string_view key)
{
    std::size_t start = 0;
    while (start < meminfo.size())
    {
        std::size_t end = meminfo.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = meminfo.size();
        }
        const std::string_view line = meminfo.substr(start, end - start);
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == ':')
        {
            const std::vector<std::string_view> fields = splitFields(line.substr(key.size() + 1));
            return fields.empty() ? std::nullopt : parseUnsigned(fields.front());
        }
        start = end + 1;
    }
    return std::nullopt;
}

/// /proc/sys/vm/overcommit_memory's text: 0 heuristic, 1 always, 2 strict.
[[nodiscard]] inline OvercommitMode parseOvercommitMode(std::string_view text)
{
    const std::vector<std::string_view> fields = splitFields(text);
    if (fields.size() != 1)
    {
        return OvercommitMode::Unknown;
    }
    switch (parseUnsigned(fields.front()).value_or(3))
    {
    case 0:
        return OvercommitMode::Heuristic;
    case 1:
        return OvercommitMode::Always;
    case 2:
        return OvercommitMode::Strict;
    default:
        return OvercommitMode::Unknown;
    }
}

/// A /proc/swaps file name with the kernel's octal escapes undone ("/swap\040file" -> "/swap file").
[[nodiscard]] inline std::string unescapeSwapPath(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    const auto isOctal = [](char c)
    {
        return c >= '0' && c <= '7';
    };
    std::size_t i = 0;
    while (i < path.size())
    {
        if (path[i] == '\\' && i + 3 < path.size() && isOctal(path[i + 1]) && isOctal(path[i + 2]) && isOctal(path[i + 3]))
        {
            const int value = ((path[i + 1] - '0') * 64) + ((path[i + 2] - '0') * 8) + (path[i + 3] - '0');
            out.push_back(static_cast<char>(value & 0xFF));
            i += 4;
            continue;
        }
        out.push_back(path[i]);
        ++i;
    }
    return out;
}

/// /proc/swaps: a header line, then "Filename Type Size Used Priority" per device, sizes in KiB. A line
/// that doesn't fit is skipped.
[[nodiscard]] inline std::vector<PageFile> parseSwaps(std::string_view text)
{
    constexpr std::uint64_t KIB = 1024;
    std::vector<PageFile> devices;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        const std::vector<std::string_view> fields = splitFields(text.substr(start, end - start));
        start = end + 1;
        if (fields.size() < 5)
        {
            continue;
        }
        const std::optional<std::uint64_t> size = parseUnsigned(fields[2]);
        const std::optional<std::uint64_t> used = parseUnsigned(fields[3]);
        int priority = 0;
        const std::string_view priorityText = fields[4];
        const auto [end2, ec] = std::from_chars(priorityText.data(), priorityText.data() + priorityText.size(), priority);
        if (!size.has_value() || !used.has_value() || ec != std::errc{} || end2 != priorityText.data() + priorityText.size())
        {
            continue; // the header, or a line this parser doesn't know
        }
        devices.push_back({
            .path = unescapeSwapPath(fields[0]),
            .kind = std::string(fields[1]),
            .sizeBytes = *size * KIB,
            .usedBytes = *used * KIB,
            .peakBytes = 0,
            .priority = priority,
        });
    }
    return devices;
}

/// /sys/block/zramN/mm_stat: orig_data_size compr_data_size mem_used_total ..., in bytes. nullopt when
/// the first three aren't numbers.
[[nodiscard]] inline std::optional<ZramDevice> parseZramMmStat(std::string_view text)
{
    const std::vector<std::string_view> fields = splitFields(text);
    if (fields.size() < 3)
    {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> original = parseUnsigned(fields[0]);
    const std::optional<std::uint64_t> compressed = parseUnsigned(fields[1]);
    const std::optional<std::uint64_t> used = parseUnsigned(fields[2]);
    if (!original.has_value() || !compressed.has_value() || !used.has_value())
    {
        return std::nullopt;
    }
    return ZramDevice{.name = {}, .originalBytes = *original, .compressedBytes = *compressed, .memoryUsedBytes = *used};
}

/// A sysfs boolean parameter ("Y", "N", "1", "0"); nullopt otherwise.
[[nodiscard]] inline std::optional<bool> parseSysfsBool(std::string_view text)
{
    const std::vector<std::string_view> fields = splitFields(text);
    if (fields.size() != 1)
    {
        return std::nullopt;
    }
    if (fields.front() == "Y" || fields.front() == "y" || fields.front() == "1")
    {
        return true;
    }
    if (fields.front() == "N" || fields.front() == "n" || fields.front() == "0")
    {
        return false;
    }
    return std::nullopt;
}

/// The bracketed choice in a sysfs selector ("always [madvise] never" -> "madvise"); empty without one.
[[nodiscard]] inline std::string parseBracketedChoice(std::string_view text)
{
    const std::size_t open = text.find('[');
    if (open == std::string_view::npos)
    {
        return {};
    }
    const std::size_t close = text.find(']', open + 1);
    if (close == std::string_view::npos)
    {
        return {};
    }
    return std::string(text.substr(open + 1, close - open - 1));
}

/// The facts under @p root into @p info.
inline void readCommitPagingFacts(const std::filesystem::path& root, CommitPagingInfo& info)
{
    constexpr std::uint64_t KIB = 1024;
    info.available = true;
    info.family = OsFamily::Linux;

    const std::string meminfo = LinuxOsInfo::readFile(root / "proc/meminfo");
    info.committedBytes = meminfoValue(meminfo, "Committed_AS").value_or(0) * KIB;
    info.commitLimitBytes = meminfoValue(meminfo, "CommitLimit").value_or(0) * KIB;
    if (const std::optional<std::uint64_t> total = meminfoValue(meminfo, "HugePages_Total"); total.has_value())
    {
        info.hugePagesRead = true;
        info.hugePagesTotal = *total;
        info.hugePagesFree = meminfoValue(meminfo, "HugePages_Free").value_or(0);
        info.hugePagesReserved = meminfoValue(meminfo, "HugePages_Rsvd").value_or(0);
        info.hugePagesSurplus = meminfoValue(meminfo, "HugePages_Surp").value_or(0);
        info.hugePageSizeBytes = meminfoValue(meminfo, "Hugepagesize").value_or(0) * KIB;
    }
    info.overcommit = parseOvercommitMode(LinuxOsInfo::readFile(root / "proc/sys/vm/overcommit_memory"));

    // /proc/swaps always has its header line, so an empty read means it couldn't be read.
    if (const std::string swaps = LinuxOsInfo::readFile(root / "proc/swaps"); !swaps.empty())
    {
        info.pageFilesRead = true;
        info.pageFiles = parseSwaps(swaps);
    }

    std::error_code ec;
    std::filesystem::directory_iterator blocks(root / "sys/block", ec);
    if (!ec)
    {
        info.zramRead = true;
        // Incremented with an error code: a range-for would throw if the listing failed part-way.
        for (; !ec && blocks != std::filesystem::directory_iterator{}; blocks.increment(ec))
        {
            const std::filesystem::path& path = blocks->path();
            const std::string name = path.filename().string();
            if (!name.starts_with("zram"))
            {
                continue;
            }
            if (std::optional<ZramDevice> device = parseZramMmStat(LinuxOsInfo::readFile(path / "mm_stat")); device.has_value())
            {
                device->name = name;
                info.zram.push_back(std::move(*device));
            }
        }
        std::ranges::sort(info.zram, {}, &ZramDevice::name);
    }

    info.zswapEnabled = parseSysfsBool(LinuxOsInfo::readFile(root / "sys/module/zswap/parameters/enabled"));
    info.transparentHugePages = parseBracketedChoice(LinuxOsInfo::readFile(root / "sys/kernel/mm/transparent_hugepage/enabled"));
}

} // namespace Platform::LinuxCommitPaging
