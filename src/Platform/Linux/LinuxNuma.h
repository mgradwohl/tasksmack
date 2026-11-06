#pragma once

// The Linux NUMA nodes (#3088), read under an injected node root (/sys/devices/system/node in the app, a
// fixture tree in tests). Each online node is a node<N> directory with
//   cpulist   its CPUs, as a kernel CPU list ("0-15,32-47"); an empty line for a CPU-less node
//   meminfo   "Node 0 MemTotal:       16316412 kB", then MemFree, MemUsed and more, one per line
// The parsers are pure; the readers do plain file reads and never throw on a missing or vanished node.
// Raw values only: used memory and per-node CPU use are the Domain layer's.

#include "Platform/AsciiTrim.h"
#include "Platform/CpuAffinity.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/SystemTypes.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::LinuxNuma
{

/// A node's memory, from its meminfo.
struct NodeMemory
{
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
};

namespace Detail
{

/// @p text as a whole unsigned number; nullopt for anything else (empty, a sign, trailing characters).
template<typename T> [[nodiscard]] inline std::optional<T> parseWhole(std::string_view text) noexcept
{
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// The next whitespace-separated field of @p rest, which is advanced past it; empty at the end.
[[nodiscard]] inline std::string_view nextField(std::string_view& rest)
{
    const std::size_t start = rest.find_first_not_of(" \t\r");
    if (start == std::string_view::npos)
    {
        rest = {};
        return {};
    }
    rest.remove_prefix(start);
    const std::size_t end = std::min(rest.find_first_of(" \t\r"), rest.size());
    const std::string_view field = rest.substr(0, end);
    rest.remove_prefix(end);
    return field;
}

/// One meminfo line, "Node <n> <Key>: <value> kB", as its key and value in bytes; nullopt when the line
/// isn't in that form (the kB unit included: a value without it can't be taken as KiB), or the value
/// doesn't fit in 64 bits as bytes. Unitless counts such as HugePages_Total are skipped with it; none is used.
struct MeminfoLine
{
    std::string_view key;
    std::uint64_t bytes = 0;
};

[[nodiscard]] inline std::optional<MeminfoLine> parseMeminfoLine(std::string_view line)
{
    constexpr std::uint64_t KIB = 1024;
    std::string_view rest = line;
    if (nextField(rest) != "Node" || !parseWhole<unsigned>(nextField(rest)).has_value())
    {
        return std::nullopt;
    }
    std::string_view key = nextField(rest);
    if (key.size() < 2 || !key.ends_with(':'))
    {
        return std::nullopt;
    }
    key.remove_suffix(1);
    const auto kib = parseWhole<std::uint64_t>(nextField(rest));
    const std::string_view unit = nextField(rest);
    if (!kib.has_value() || unit != "kB" || !nextField(rest).empty() || *kib > std::numeric_limits<std::uint64_t>::max() / KIB)
    {
        return std::nullopt;
    }
    return MeminfoLine{.key = key, .bytes = *kib * KIB};
}

} // namespace Detail

/// A node's meminfo text: MemTotal, and MemFree (or, without it, MemTotal - MemUsed, which is how the
/// kernel computes MemUsed). Malformed lines are skipped. nullopt without a usable MemTotal and free
/// figure. Free is capped at the total, so used memory can't come out negative.
[[nodiscard]] inline std::optional<NodeMemory> parseNodeMeminfo(std::string_view text)
{
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> free;
    std::optional<std::uint64_t> used;
    while (!text.empty())
    {
        const std::size_t newline = std::min(text.find('\n'), text.size());
        const auto line = Detail::parseMeminfoLine(text.substr(0, newline));
        text.remove_prefix(std::min(newline + 1, text.size()));
        if (!line.has_value())
        {
            continue;
        }
        // The first of each key counts; the kernel prints each once.
        if (line->key == "MemTotal" && !total.has_value())
        {
            total = line->bytes;
        }
        else if (line->key == "MemFree" && !free.has_value())
        {
            free = line->bytes;
        }
        else if (line->key == "MemUsed" && !used.has_value())
        {
            used = line->bytes;
        }
    }
    if (!total.has_value())
    {
        return std::nullopt;
    }
    if (!free.has_value() && used.has_value())
    {
        free = *total - std::min(*used, *total);
    }
    if (!free.has_value())
    {
        return std::nullopt;
    }
    return NodeMemory{.totalBytes = *total, .freeBytes = std::min(*free, *total)};
}

/// A node's cpulist text: its CPUs, empty for a CPU-less node (the kernel prints an empty line). nullopt
/// when the list is malformed (CpuAffinity::fromCpuList's rules).
[[nodiscard]] inline std::optional<CpuAffinity> parseNodeCpuList(std::string_view text)
{
    if (trimAscii(text).empty())
    {
        return CpuAffinity{};
    }
    return CpuAffinity::fromCpuList(text);
}

/// The node number of a node root entry named "node<N>"; nullopt for any other name ("online",
/// "has_cpu", "node", "node-1", "nodeX").
[[nodiscard]] inline std::optional<int> nodeIdFromName(std::string_view name)
{
    constexpr std::string_view PREFIX = "node";
    if (!name.starts_with(PREFIX))
    {
        return std::nullopt;
    }
    // Parsed unsigned, since from_chars would take "-1" as an int.
    const auto id = Detail::parseWhole<unsigned>(name.substr(PREFIX.size()));
    if (!id.has_value() || *id > static_cast<unsigned>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }
    return static_cast<int>(*id);
}

#if defined(__linux__) && __has_include(<unistd.h>)

/// The online nodes' numbers under @p nodeRoot, ascending; empty when it can't be listed (a kernel
/// without CONFIG_NUMA has no node root).
[[nodiscard]] inline std::vector<int> listNodeIds(const std::filesystem::path& nodeRoot)
{
    std::vector<int> ids;
    std::error_code ec;
    std::filesystem::directory_iterator entries(nodeRoot, ec);
    // Incremented with an error code: a range-for would throw if the listing failed part-way.
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        if (const auto id = nodeIdFromName(entries->path().filename().string()); id.has_value())
        {
            ids.push_back(*id);
        }
    }
    std::ranges::sort(ids);
    return ids;
}

/// <nodeRoot>/node<id>.
[[nodiscard]] inline std::filesystem::path nodeDir(const std::filesystem::path& nodeRoot, int id)
{
    return nodeRoot / ("node" + std::to_string(id));
}

/// Node @p id's CPUs; nullopt when its cpulist can't be read or is malformed.
[[nodiscard]] inline std::optional<CpuAffinity> readNodeCpus(const std::filesystem::path& nodeRoot, int id)
{
    const std::vector<char> text = ProcParsing::readProcFileFull((nodeDir(nodeRoot, id) / "cpulist").c_str());
    if (text.empty())
    {
        return std::nullopt;
    }
    return parseNodeCpuList(std::string_view(text.data(), text.size()));
}

/// Node @p id's memory; nullopt when its meminfo can't be read (the node went offline) or parsed.
[[nodiscard]] inline std::optional<NodeMemory> readNodeMemory(const std::filesystem::path& nodeRoot, int id)
{
    const std::vector<char> text = ProcParsing::readProcFileFull((nodeDir(nodeRoot, id) / "meminfo").c_str());
    return parseNodeMeminfo(std::string_view(text.data(), text.size()));
}

/// Every node under @p nodeRoot, ascending by id, with its CPUs and memory. A node whose meminfo can't be
/// read is left out; one whose cpulist can't be read keeps empty CPUs.
[[nodiscard]] inline std::vector<NumaNode> readNodes(const std::filesystem::path& nodeRoot)
{
    std::vector<NumaNode> nodes;
    for (const int id : listNodeIds(nodeRoot))
    {
        const auto memory = readNodeMemory(nodeRoot, id);
        if (!memory.has_value())
        {
            continue;
        }
        nodes.push_back(NumaNode{
            .id = id,
            .cpus = readNodeCpus(nodeRoot, id).value_or(CpuAffinity{}),
            .memTotalBytes = memory->totalBytes,
            .memFreeBytes = memory->freeBytes,
        });
    }
    return nodes;
}

#endif

} // namespace Platform::LinuxNuma
