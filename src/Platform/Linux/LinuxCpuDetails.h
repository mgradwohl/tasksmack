#pragma once

// The Linux CPU Details facts (#809), read from the proc and CPU sysfs roots LinuxSystemProbe was
// given (#1351), so tests point them at fixture trees:
// - sockets, physical cores and logical processors from /proc/cpuinfo, which lists the online
//   CPUs; their numbers are the online set the sysfs scans below are limited to;
// - cache totals from the online CPUs' cpuN/cache/index* (each shared instance counted once), and
//   per-processor efficiency classes from their cpuN/cpu_capacity;
// - the base clock: the highest base_frequency across cpufreq/policy* and the cpuN/cpufreq links.
// LinuxSystemProbe reads them when it is built and again when the set of CPUs in /proc/stat changes,
// committing a re-read only when the online CPUs it describes are that sample's (commitIfConsistent()).
//
// Only standard-library file access, no POSIX headers: the parsing and the fixture tests build and
// run on every platform.

#include "Platform/CpuDetails.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Platform::LinuxCpuDetails
{

namespace Detail
{

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept
{
    constexpr std::string_view SPACE = " \t\r\n";
    const auto first = text.find_first_not_of(SPACE);
    if (first == std::string_view::npos)
    {
        return {};
    }
    // remove_prefix/remove_suffix, not substr(), which can throw: this is noexcept
    const auto last = text.find_last_not_of(SPACE);
    text.remove_suffix(text.size() - last - 1);
    text.remove_prefix(first);
    return text;
}

[[nodiscard]] inline std::optional<std::uint64_t> parseUnsigned(std::string_view text) noexcept
{
    text = trim(text);
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// The whole file, or nullopt if it cannot be opened.
[[nodiscard]] inline std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace Detail

/// One /proc/cpuinfo "processor" block's topology fields; each is nullopt where the block has none.
struct CpuInfoProcessor
{
    std::optional<std::size_t> processor; ///< The logical CPU number N (sysfs cpuN)
    std::optional<std::uint64_t> physicalId;
    std::optional<std::uint64_t> coreId;
    std::optional<std::uint64_t> cpuCores; ///< Cores in this processor's socket
};

/// /proc/cpuinfo's text as one record per "processor" line, in order. A block starts at its
/// "processor" line; any other "key : value" line fills in the current block's topology fields,
/// and lines before the first block (or without a colon) are ignored.
[[nodiscard]] inline std::vector<CpuInfoProcessor> parseCpuInfoProcessors(std::string_view text)
{
    std::vector<CpuInfoProcessor> processors;
    while (!text.empty())
    {
        // Take the next line off the front of the text.
        const auto newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = (newline == std::string_view::npos) ? std::string_view{} : text.substr(newline + 1);

        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue; // The blank line between blocks
        }
        const std::string_view key = Detail::trim(line.substr(0, colon));
        const std::string_view value = line.substr(colon + 1);
        if (key == "processor")
        {
            const auto number = Detail::parseUnsigned(value);
            processors.push_back(
                {.processor = number.has_value() ? std::optional<std::size_t>{static_cast<std::size_t>(*number)} : std::nullopt,
                 .physicalId = std::nullopt,
                 .coreId = std::nullopt,
                 .cpuCores = std::nullopt});
            continue;
        }
        if (processors.empty())
        {
            continue;
        }
        CpuInfoProcessor& current = processors.back();
        if (key == "physical id")
        {
            current.physicalId = Detail::parseUnsigned(value);
        }
        else if (key == "core id")
        {
            current.coreId = Detail::parseUnsigned(value);
        }
        else if (key == "cpu cores")
        {
            current.cpuCores = Detail::parseUnsigned(value);
        }
    }
    return processors;
}

/// Sockets, physical cores and logical processors from the processor records. Each record is one
/// logical processor; sockets are the distinct "physical id"s and cores the distinct
/// ("physical id", "core id") pairs. Where a processor has no "core id" (some VMs), cores are the
/// sum of each socket's "cpu cores", if every socket gives one. If any processor lists no
/// "physical id" (most ARM lists none) sockets and cores stay unknown: counting only the processors
/// that have one would undercount both. A hybrid CPU's cores are counted, not split: /proc/cpuinfo
/// does not say which are which.
inline void summarizeCpuInfoTopology(std::span<const CpuInfoProcessor> processors, CpuDetails& details)
{
    if (processors.empty())
    {
        return;
    }
    details.logicalProcessors = processors.size();
    if (!std::ranges::all_of(processors, [](const CpuInfoProcessor& processor) { return processor.physicalId.has_value(); }))
    {
        return;
    }

    std::set<std::uint64_t> sockets;
    std::set<std::pair<std::uint64_t, std::uint64_t>> cores;
    std::map<std::uint64_t, std::uint64_t> coresPerSocket; // The first "cpu cores" listed for each socket
    bool everyProcessorHasCoreId = true;
    for (const CpuInfoProcessor& processor : processors)
    {
        const std::uint64_t physicalId = processor.physicalId.value_or(0); // Every processor has one (checked above)
        sockets.insert(physicalId);
        if (processor.coreId.has_value())
        {
            cores.emplace(physicalId, *processor.coreId);
        }
        else
        {
            everyProcessorHasCoreId = false;
        }
        if (processor.cpuCores.has_value())
        {
            coresPerSocket.emplace(physicalId, *processor.cpuCores);
        }
    }
    details.sockets = sockets.size();

    if (everyProcessorHasCoreId)
    {
        details.physicalCores = cores.size();
        return;
    }
    // No core ids: fall back to "cpu cores", but only when every socket reported a non-zero count.
    const bool everySocketCounted = coresPerSocket.size() == sockets.size() &&
                                    std::ranges::all_of(coresPerSocket, [](const auto& socketCores) { return socketCores.second > 0; });
    if (everySocketCounted)
    {
        std::uint64_t sum = 0;
        for (const auto& [socket, count] : coresPerSocket)
        {
            sum += count;
        }
        details.physicalCores = static_cast<std::size_t>(sum);
    }
}

/// Sockets, physical cores and logical processors from /proc/cpuinfo's text
/// (parseCpuInfoProcessors(), then summarizeCpuInfoTopology()).
inline void parseCpuInfoTopology(std::string_view text, CpuDetails& details)
{
    const std::vector<CpuInfoProcessor> processors = parseCpuInfoProcessors(text);
    summarizeCpuInfoTopology(processors, details);
}

/// The online CPUs' numbers: the processors /proc/cpuinfo lists (it lists online CPUs only).
/// nullopt when there are none, or any has no number, and the sysfs scans then take every cpuN.
[[nodiscard]] inline std::optional<std::set<std::size_t>> onlineCpus(std::span<const CpuInfoProcessor> processors)
{
    std::set<std::size_t> online;
    for (const CpuInfoProcessor& processor : processors)
    {
        if (!processor.processor.has_value())
        {
            return std::nullopt;
        }
        online.insert(*processor.processor);
    }
    return online.empty() ? std::nullopt : std::optional<std::set<std::size_t>>{std::move(online)};
}

/// Whether sysfs's cpuN is one of `online` (every cpuN when the online set is unknown). sysfs lists
/// possible CPUs, offline ones too; their caches and capacities would describe a different set of
/// CPUs from the topology /proc/cpuinfo gives.
[[nodiscard]] inline bool isOnline(const std::optional<std::set<std::size_t>>& online, std::size_t cpu)
{
    return !online.has_value() || online->contains(cpu);
}

/// A sysfs cache size ("32K", "1024K", "8M", "16384 KB") in bytes; nullopt if unreadable or zero.
[[nodiscard]] inline std::optional<std::uint64_t> parseCacheSize(std::string_view text) noexcept
{
    text = Detail::trim(text);
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr == text.data() || value == 0)
    {
        return std::nullopt;
    }
    std::string_view suffix = text;
    suffix.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    suffix = Detail::trim(suffix);
    if (suffix.ends_with('B') || suffix.ends_with('b'))
    {
        suffix.remove_suffix(1);
    }
    std::uint64_t scale = 1;
    if (suffix.empty())
    {
        scale = 1;
    }
    else if (suffix == "K" || suffix == "k")
    {
        scale = 1024;
    }
    else if (suffix == "M" || suffix == "m")
    {
        scale = 1024ULL * 1024ULL;
    }
    else if (suffix == "G" || suffix == "g")
    {
        scale = 1024ULL * 1024ULL * 1024ULL;
    }
    else
    {
        return std::nullopt;
    }
    return value * scale;
}

/// One cpuN/cache/indexM directory's contents.
struct SysfsCacheEntry
{
    std::size_t cpu = 0; ///< N of the cpuN it was read under.
    unsigned level = 0;
    std::string type;          ///< "Data", "Instruction" or "Unified".
    std::uint64_t bytes = 0;   ///< One instance's size.
    std::string sharedCpuList; ///< The CPUs sharing this instance ("0-1,8-9"); empty if unreadable.
};

/// The distinct cache instances among `entries`. Every CPU sharing a cache lists it, so instances
/// are told apart by level, type and the CPUs sharing them. An entry with no shared list cannot be
/// matched with its siblings and is taken as private to its CPU.
[[nodiscard]] inline std::vector<CpuTopology::CacheInstance> distinctCacheInstances(std::span<const SysfsCacheEntry> entries)
{
    std::set<std::tuple<unsigned, std::string, std::string>> seen;
    std::vector<CpuTopology::CacheInstance> instances;
    for (const SysfsCacheEntry& entry : entries)
    {
        if (entry.bytes == 0)
        {
            continue;
        }
        std::string sharedBy = entry.sharedCpuList.empty() ? ("cpu" + std::to_string(entry.cpu)) : entry.sharedCpuList;
        if (seen.emplace(entry.level, entry.type, std::move(sharedBy)).second)
        {
            instances.push_back({.level = entry.level, .bytes = entry.bytes});
        }
    }
    return instances;
}

/// The base clock in MHz from cpufreq's base_frequency in kHz (the rated clock, as intel_pstate and
/// amd-pstate report it); nullopt when there is none or it is under 1 MHz. bios_limit and
/// cpuinfo_max_freq are not used: they are maximums (the boost clock on most drivers), not the base.
[[nodiscard]] inline std::optional<std::uint64_t> baseSpeedMHzFromKHz(std::optional<std::uint64_t> baseFrequencyKHz) noexcept
{
    if (baseFrequencyKHz.has_value() && *baseFrequencyKHz >= 1000)
    {
        return *baseFrequencyKHz / 1000;
    }
    return std::nullopt;
}

/// N of a "cpuN" directory name; nullopt for anything else (cpufreq, cpuidle, ...).
[[nodiscard]] inline std::optional<std::size_t> cpuDirectoryIndex(std::string_view name) noexcept
{
    if (!name.starts_with("cpu") || name.size() == 3)
    {
        return std::nullopt;
    }
    name.remove_prefix(3);
    std::size_t index = 0;
    const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), index);
    if (ec != std::errc{} || ptr != name.data() + name.size())
    {
        return std::nullopt;
    }
    return index;
}

/// Every online cpuN/cache/index* entry under `cpuSysfsRoot`. Missing directories and unreadable
/// files leave entries out rather than fail.
[[nodiscard]] inline std::vector<SysfsCacheEntry> readCacheEntries(const std::filesystem::path& cpuSysfsRoot,
                                                                   const std::optional<std::set<std::size_t>>& online)
{
    std::vector<SysfsCacheEntry> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator cpuIt(cpuSysfsRoot, ec), end; !ec && cpuIt != end; cpuIt.increment(ec))
    {
        const auto cpu = cpuDirectoryIndex(cpuIt->path().filename().string());
        if (!cpu.has_value() || !isOnline(online, *cpu))
        {
            continue;
        }
        std::error_code cacheEc;
        for (std::filesystem::directory_iterator indexIt(cpuIt->path() / "cache", cacheEc), indexEnd; !cacheEc && indexIt != indexEnd;
             indexIt.increment(cacheEc))
        {
            if (!indexIt->path().filename().string().starts_with("index"))
            {
                continue;
            }
            const auto level = Detail::readFile(indexIt->path() / "level");
            const auto size = Detail::readFile(indexIt->path() / "size");
            const auto levelValue = level.has_value() ? Detail::parseUnsigned(*level) : std::nullopt;
            const auto bytes = size.has_value() ? parseCacheSize(*size) : std::nullopt;
            if (!levelValue.has_value() || !bytes.has_value())
            {
                continue;
            }
            const auto type = Detail::readFile(indexIt->path() / "type");
            const auto shared = Detail::readFile(indexIt->path() / "shared_cpu_list");
            entries.push_back({.cpu = *cpu,
                               .level = static_cast<unsigned>(*levelValue),
                               .type = type.has_value() ? std::string(Detail::trim(*type)) : std::string{},
                               .bytes = *bytes,
                               .sharedCpuList = shared.has_value() ? std::string(Detail::trim(*shared)) : std::string{}});
        }
    }
    // Directory order is unspecified; sort so the result does not depend on it.
    std::ranges::sort(entries,
                      [](const SysfsCacheEntry& a, const SysfsCacheEntry& b)
                      { return std::tie(a.cpu, a.level, a.type) < std::tie(b.cpu, b.level, b.type); });
    return entries;
}

/// Each logical processor's efficiency class from cpuN/cpu_capacity (#809), where the kernel gives
/// one (Arm big.LITTLE, recent x86 hybrids): the rank of its capacity among the distinct capacities,
/// lowest 0, online CPUs only. Empty unless there is more than one capacity.
[[nodiscard]] inline std::vector<std::uint8_t> readEfficiencyClasses(const std::filesystem::path& cpuSysfsRoot,
                                                                     const std::optional<std::set<std::size_t>>& online)
{
    std::vector<std::pair<std::size_t, std::uint64_t>> capacities;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(cpuSysfsRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        const auto cpu = cpuDirectoryIndex(it->path().filename().string());
        const auto text = (cpu.has_value() && isOnline(online, *cpu)) ? Detail::readFile(it->path() / "cpu_capacity") : std::nullopt;
        const auto capacity = text.has_value() ? Detail::parseUnsigned(*text) : std::nullopt;
        if (capacity.has_value())
        {
            capacities.emplace_back(*cpu, *capacity);
        }
    }
    std::set<std::uint64_t> distinct;
    for (const auto& entry : capacities)
    {
        distinct.insert(entry.second);
    }
    std::vector<std::uint8_t> classes;
    for (const auto& [cpu, capacity] : capacities)
    {
        const auto rank = std::distance(distinct.begin(), distinct.find(capacity));
        CpuTopology::setEfficiencyClass(
            classes, cpu, static_cast<std::uint8_t>(std::min<std::ptrdiff_t>(rank, UNKNOWN_EFFICIENCY_CLASS - 1)));
    }
    CpuTopology::keepOnlyIfHybrid(classes);
    return classes;
}

/// The base clock in MHz: the highest base_frequency of any cpufreq policy (#809). base_frequency is
/// per policy, and on a hybrid CPU cpu0 may sit in an efficiency-core policy with a lower base; the
/// highest is the performance cores' rated clock. Both cpufreq/policy*/ and each cpuN/cpufreq/ (a
/// link to its policy on a real system) are read. nullopt when no policy has one.
[[nodiscard]] inline std::optional<std::uint64_t> readBaseSpeedMHz(const std::filesystem::path& cpuSysfsRoot)
{
    std::optional<std::uint64_t> highestKHz;
    const auto consider = [&highestKHz](const std::filesystem::path& file)
    {
        const auto text = Detail::readFile(file);
        const auto kHz = text.has_value() ? Detail::parseUnsigned(*text) : std::nullopt;
        if (kHz.has_value() && (!highestKHz.has_value() || *kHz > *highestKHz))
        {
            highestKHz = kHz;
        }
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it(cpuSysfsRoot / "cpufreq", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().filename().string().starts_with("policy"))
        {
            consider(it->path() / "base_frequency");
        }
    }
    std::error_code cpuEc;
    for (std::filesystem::directory_iterator it(cpuSysfsRoot, cpuEc), end; !cpuEc && it != end; it.increment(cpuEc))
    {
        if (cpuDirectoryIndex(it->path().filename().string()).has_value())
        {
            consider(it->path() / "cpufreq" / "base_frequency");
        }
    }
    return baseSpeedMHzFromKHz(highestKHz);
}

/// The CPU Details facts from `procRoot`/cpuinfo and `cpuSysfsRoot` (normally /proc and
/// /sys/devices/system/cpu). Whatever cannot be read stays nullopt. `onlineCpuIds`, if given,
/// receives the online CPUs' numbers the facts describe, ascending (empty when /proc/cpuinfo gives
/// none).
[[nodiscard]] inline CpuDetails
read(const std::filesystem::path& procRoot, const std::filesystem::path& cpuSysfsRoot, std::vector<std::size_t>* onlineCpuIds = nullptr)
{
    CpuDetails details;
    std::vector<CpuInfoProcessor> processors;
    if (const auto cpuInfo = Detail::readFile(procRoot / "cpuinfo"); cpuInfo.has_value())
    {
        processors = parseCpuInfoProcessors(*cpuInfo);
    }
    summarizeCpuInfoTopology(processors, details);
    // One coherent set of CPUs: the sysfs scans take only the CPUs /proc/cpuinfo listed as online
    const std::optional<std::set<std::size_t>> online = onlineCpus(processors);
    if (onlineCpuIds != nullptr)
    {
        onlineCpuIds->clear();
        if (online.has_value())
        {
            onlineCpuIds->assign(online->begin(), online->end());
        }
    }

    const std::vector<SysfsCacheEntry> entries = readCacheEntries(cpuSysfsRoot, online);
    const std::vector<CpuTopology::CacheInstance> instances = distinctCacheInstances(entries);
    CpuTopology::sumCacheInstances(instances, details);
    details.efficiencyClassByCoreId = readEfficiencyClasses(cpuSysfsRoot, online);

    details.baseSpeedMHz = readBaseSpeedMHz(cpuSysfsRoot);
    return details;
}

} // namespace Platform::LinuxCpuDetails
